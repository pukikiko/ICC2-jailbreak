/*
 * rawplay's usb transport: the vendor interface of the composite livi gadget, but with
 * bulk-in urbs that can point straight at the caller's memory (the panel mapping) instead
 * of always copying out of a fixed dma buffer. see rawplay/README.md section 2.3.
 *
 * the same descriptor walk as livi-usb.c: find the 0xff interface with two bulk endpoints,
 * open both pipes. the read side keeps several urbs in flight; a completion that is short
 * of its request is compacted down to where the stream actually continues, so the bytes
 * always land in stream order even if the gadget hands back a short packet.
 */
#ifndef RAW_USB_H
#define RAW_USB_H

/* connect to io-usb, attach the vendor interface and open both pipes. waits for the
 * gadget for a few seconds. 0 on success, -1 if it never shows. */
int raw_usb_start(void);

/* read exactly n bytes into dst, return 0. uses direct urbs when raw_usb_set_direct(1)
 * was called and the stack accepted them; otherwise a usbd_alloc staging ring is copied
 * out of. timeout_ms is the longest a read may go without a byte before it gives up:
 * -1 is returned, raw_usb_quiet() says whether it was the quiet link or a dead device. */
int raw_usb_read(void *dst, int n, int timeout_ms);

/* ask the transport to give up waiting: the read in progress returns -1 at once. the exit
 * gesture uses this, so a link that never delivers a byte can still be left without
 * killing the app (the urbs are never aborted: this stack segfaults on that). */
void raw_usb_stop(void);
/* 1 when the last read ended because nothing arrived inside its timeout, 0 for a dead
 * device or a stop. the caller tells "no video from the host" from "link read failed". */
int raw_usb_quiet(void);
/* 1 when a bulk-in completion errored since the last resync: the current frame is lost
 * mid-transfer. the caller drops it (and acks it stale), calls raw_usb_resync() and picks
 * up scanning for the next LR header. */
int raw_usb_error(void);
void raw_usb_resync(void);
/* the bulk-in endpoint's max packet (512 high speed, 64 full speed). every read must be
 * a whole number of these: the device sends maxpacket-sized packets, and a request that
 * is not a multiple of it makes the host drop the tail of the last packet (and the ehci
 * side error the transfer), which on a real car is what "no video" looked like. */
int raw_usb_mps(void);

/* the size of one bulk-in urb, default 256 KB. a real unit's ehci driver can behave
 * differently from qemu's model, so the player's --urb sets it (rounded down to a
 * maxpacket multiple, clamped to the staging ring). */
void raw_usb_set_chunk(int bytes);
int raw_usb_chunk(void);

/* enable the zero-copy path: bulk-in urbs are set up with dst as the transfer buffer.
 * raw_usb_read falls back to the staging ring by itself if usbd_setup_bulk/usbd_io
 * rejects the buffer, and raw_usb_direct() then reports 0. */
void raw_usb_set_direct(int on);
int raw_usb_direct(void);
/* 1 once a completion came back short of its request and had to be compacted; the
 * benchmark prints this so a staged run is never mistaken for a zero-copy one. */
int raw_usb_compacted(void);

/* the bulk-out pipe (touch/acks), chunked the way livi-usb.c does it */
int raw_usb_write(const void *src, int n);

/* a usbd_alloc'd RAW_CHUNK scratch buffer, for discarding a superseded frame without
 * dragging the dma ring onto a non-dma buffer (which would disable direct mode). */
void *raw_usb_scratch(void);
int raw_usb_scratch_size(void);

#endif
