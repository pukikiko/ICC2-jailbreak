#!/usr/bin/env python3
# input injection into the display the raw streamer is capturing: the weston-touch module's
# socket (rawplay/weston-touch.c) when it is there, and xtest pointer/key events otherwise.
# this is the shared piece the raw host (rawplay/rawlink.c) uses; it has no video codec and
# no wire protocol of its own.
import ctypes, os, socket, sys


def log(msg):
    print(msg, file=sys.stderr, flush=True)


# where the weston-touch module listens; same default as rawplay/weston-touch.c
TOUCH_SOCK = os.environ.get('LIVI_TOUCH_SOCK', '/tmp/livi-touch.sock')


class TouchSocket:
    """the weston-touch module's socket (rawplay/weston-touch.c): the compositor gets a real
    touch device and the app under it sees a touchscreen, not a mouse, so there is no
    cursor and drags are touch drags. a touch sample is just a line, and a restarted module
    (or streamer) resynchronises because every command stands alone."""

    def __init__(self, path):
        self.path = path
        self.sock = None
        self.down = False
        self.connect()

    def connect(self):
        s = socket.socket(socket.AF_UNIX)
        s.connect(self.path)
        self.sock = s
        self.down = False
        log(f'inject: touch injection on {self.path}')

    def touch(self, down, x, y):
        if down:
            self._send(('m' if self.down else 'd') + f' {x} {y}\n')
            self.down = True
        elif self.down:
            self.down = False
            self._send('u\n')

    def _send(self, cmd):
        if self.sock is None:
            raise OSError('touch socket lost')
        try:
            self.sock.sendall(cmd.encode())
        except OSError:
            # weston restarted: reconnect once, then give up for this session
            self.sock = None
            try:
                self.connect()
                if self.down and cmd.startswith('m'):
                    cmd = 'd' + cmd[1:]     # the new connection has no touch down yet
                self.sock.sendall(cmd.encode())
            except OSError as e:
                log(f'inject: touch socket lost ({e})')
                self.sock = None
                raise


class XInput:
    """xtest pointer/key events into the display the capturer is reading: the touch
    fallback for when weston is not running livi/weston-touch.so, and the path the knob
    and panel buttons always use. when the module's socket is there, touch is real wl_touch
    and the pointer is never moved, which is what keeps the cursor away.

    ctypes rather than xdotool: one process per event would add its own startup to every
    sample."""

    def __init__(self, display, touch_sock=None):
        self.x11 = ctypes.CDLL('libX11.so.6')
        self.xtst = ctypes.CDLL('libXtst.so.6')
        self.x11.XOpenDisplay.restype = ctypes.c_void_p
        self.x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
        self.x11.XFlush.argtypes = [ctypes.c_void_p]
        self.x11.XKeysymToKeycode.restype = ctypes.c_ubyte
        self.x11.XKeysymToKeycode.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
        self.xtst.XTestFakeMotionEvent.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
                                                   ctypes.c_int, ctypes.c_ulong]
        self.xtst.XTestFakeButtonEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int,
                                                   ctypes.c_ulong]
        self.xtst.XTestFakeKeyEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int,
                                                ctypes.c_ulong]
        self.dpy = self.x11.XOpenDisplay(display.encode())
        if not self.dpy:
            raise RuntimeError(f'cannot open display {display}')
        self.touchsock = touch_sock
        self.down = False
        log(f'inject: input injection on {display}'
            + ('' if touch_sock else ' (mouse: weston-touch module not running)'))

    def touch(self, down, x, y):
        if self.touchsock:
            try:
                self.touchsock.touch(down, x, y)
                return
            except OSError:
                self.touchsock = None      # fall back to the mouse for the rest
        self.xtst.XTestFakeMotionEvent(self.dpy, -1, x, y, 0)
        # a press only on the 0->1 edge: the guest repeats samples while a finger is held,
        # and pressing an already pressed button reads as a double click to the app
        if bool(down) != self.down:
            self.xtst.XTestFakeButtonEvent(self.dpy, 1, 1 if down else 0, 0)
            self.down = bool(down)
        self.x11.XFlush(self.dpy)

    def key(self, keysym, down):
        code = self.x11.XKeysymToKeycode(self.dpy, keysym)
        if code:
            self.xtst.XTestFakeKeyEvent(self.dpy, code, 1 if down else 0, 0)
            self.x11.XFlush(self.dpy)

    def tap(self, keysym):
        self.key(keysym, 1)
        self.key(keysym, 0)
