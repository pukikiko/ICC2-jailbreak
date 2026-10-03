#!/usr/bin/env python3
# rawsink - a Linux PC (or the emulator's host) as the other end of rawlink's
# vendor interface: read the LR stream, ack each frame with LI type 8, send
# the ready heartbeat, and report sustained MB/s and fps. Use it to measure
# the H618 MUSB throughput the brief asks about:
#
#   board (debug image, console):  systemctl stop rawlink
#                                 /opt/livi/rawlink run --test --fps 60 --stats
#   host:                          python3 tests/rawsink.py --seconds 30
#
# The frame size is fixed at 800x480 RGB565 (768000 B). pyusb is required
# (pip install pyusb) and the user needs usbfs access; run it as root if the
# udev rules are not set up.
import argparse
import struct
import sys
import time

try:
    import usb.core
    import usb.util
except ImportError:
    sys.exit("rawsink: pyusb is required (pip install pyusb)")

VID = 0x1209
PID = 0x1cc2
MSG_HDR = 512
LR_FRAME = 2
LI_READY = 5
LI_RAW = 8


def find_vendor_endpoints(dev):
    cfg = dev.get_active_configuration()
    for itf in cfg:
        if (itf.bInterfaceClass, itf.bInterfaceSubClass,
                itf.bInterfaceProtocol) != (0xff, 0xff, 0xff):
            continue
        ep_in = ep_out = None
        for ep in itf:
            if usb.util.endpoint_type(ep.bmAttributes) == usb.util.ENDPOINT_TYPE_BULK:
                if usb.util.endpoint_direction(ep.bEndpointAddress) == usb.util.ENDPOINT_IN:
                    ep_in = ep.bEndpointAddress
                else:
                    ep_out = ep.bEndpointAddress
        return itf.bInterfaceNumber, ep_in, ep_out
    return None, None, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=30.0)
    ap.add_argument("--frame-size", type=int, default=800 * 480 * 2)
    args = ap.parse_args()

    dev = usb.core.find(idVendor=VID, idProduct=PID)
    if dev is None:
        sys.exit("rawsink: gadget %04x:%04x not found" % (VID, PID))
    itf, ep_in, ep_out = find_vendor_endpoints(dev)
    if itf is None:
        sys.exit("rawsink: no vendor bulk interface")
    usb.util.claim_interface(dev, itf)
    print("rawsink: vendor interface %d, ep %02x/%02x" % (itf, ep_in, ep_out))

    li_ready = bytes((ord('L'), ord('I'), LI_READY))
    dev.write(ep_out, li_ready)

    buf = bytearray()
    frames = 0
    bytes_seen = 0
    start = time.monotonic()
    last = start
    last_bytes = 0
    next_ready = start + 1.0
    while time.monotonic() - start < args.seconds:
        now = time.monotonic()
        if now >= next_ready:
            dev.write(ep_out, li_ready)
            next_ready = now + 1.0
        try:
            chunk = dev.read(ep_in, 512 * 1024, timeout=1000)
        except usb.core.USBTimeoutError:
            continue
        except usb.core.USBError as e:
            sys.exit("rawsink: read failed: %s" % e)
        if not chunk:
            continue
        buf += bytes(chunk)
        bytes_seen += len(chunk)
        frames += drain(buf, dev, ep_out, args.frame_size)
        now = time.monotonic()
        if now - last >= 1.0:
            rate = (bytes_seen - last_bytes) / (now - last) / 1e6
            print("rawsink: %.1f MB/s  %u frames  %.1f fps"
                  % (rate, frames, frames / (now - start)))
            last, last_bytes = now, bytes_seen

    elapsed = time.monotonic() - start
    total = frames * args.frame_size
    print("rawsink: %u frames, %.1f MB in %.1fs: %.2f MB/s, %.1f fps"
          % (frames, total / 1e6, elapsed, total / elapsed / 1e6,
             frames / elapsed))
    print("rawsink: need 11.5 MB/s for 15 fps, 23 MB/s for 30 fps")
    usb.util.release_interface(dev, itf)


def drain(buf, dev, ep_out, frame_size):
    """consume whole FRAME messages; ack each so the sender's window moves."""
    frames = 0
    i = 0
    while i + 8 <= len(buf):
        if buf[i:i + 2] != b"LR" or buf[i + 2] != LR_FRAME:
            i += 1
            continue
        length = struct.unpack_from("<I", buf, i + 4)[0]
        seq = struct.unpack_from("<I", buf, i + 8)[0]
        total = MSG_HDR + length
        if length != frame_size or i + total > len(buf):
            break
        dev.write(ep_out, struct.pack("<BBBI", ord('L'), ord('I'), LI_RAW, seq) + b"\x01")
        frames += 1
        i += total
    del buf[:i]
    return frames


if __name__ == "__main__":
    main()
