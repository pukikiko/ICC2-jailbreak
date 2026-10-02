/*
 * the unit side of the composite livi gadget: the shared usb transport for the clients
 * that ride its vendor interface (rawplay/rawplay.c today).
 *
 * the gadget (rawplay/out/rawlink gadget on a raspberry pi, or qemu's usb-livi in the emulator)
 * presents device class 0x00 with two interfaces: a vendor specific one (class 0xff, two
 * bulk endpoints) for the byte stream and touch, and a mass storage one that devb-umass
 * mounts at /fs/usb0 for the homebrew stick. this file finds the vendor interface with the
 * usb ddk (libusbdi), opens both bulk pipes and turns them into the read/write shape the
 * clients already speak.
 *
 * the insertion callback runs on a thread libusbdi creates, so the pipes are set up there
 * and the main thread just polls a flag. urb completion also happens on that thread; the
 * reader waits for a flag with a bounded sleep (no pthread types are available to us, and
 * this link has no wakeup path of its own worth a sleepon).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

#include "qnx.h"
#include "usbdi.h"
#include "livi-usb.h"

#define USB_RX_MAX      16384
#define USB_TX_MAX      512
#define USB_TIMEOUT_MS  200
#define USB_CLASS_VENDOR 0xff
#define USB_XFER_BULK   2
/* bulk-in urbs kept in flight. qemu's ehci cannot retire the rest of a multi-qtd urb on a
 * short packet, so a read has to be one 16 KB qtd; an 800x480 rgb565 frame is 768 KB and a
 * blit frame is a few of them, and with one outstanding at a time every 16 KB is a guest
 * round trip. four in flight lets the stack drain them together, which is what the large
 * frames need (the real unit does not have the qtd problem, but four urbs is no loss
 * there either). */
#define USB_RX_URBS     4

static usbd_connection_t *conn;
static usbd_device_t *udev;
static usbd_pipe_t *pipe_in, *pipe_out;
static urb_t *urb_in[USB_RX_URBS], *urb_out;
static unsigned char *buf_in[USB_RX_URBS], *buf_out;
static volatile int have_dev;
static volatile int dead;
/* the instance we attached to, so a removal for another interface is ignored */
static int dev_path = -1, dev_no = -1;

/* urb completion state, written on the libusbdi event thread and read by the caller. the
 * two directions are kept apart: a heartbeat going out while the reader has an in flight
 * must not be mistaken for the in's completion. each in urb carries its ring index as the
 * handle, so completions can land out of order without being mixed up. */
static volatile int in_done[USB_RX_URBS], out_done;
static volatile uint32_t in_status[USB_RX_URBS], in_len[USB_RX_URBS], out_status, out_len;
/* the ring: urbs up to in_tail are submitted, in_head is the oldest one the caller still
 * has to consume. bulk transfers complete in order, so consuming the oldest keeps the byte
 * stream in order even if a later completion lands first. */
static int in_head, in_tail, in_flight;

static void in_complete(urb_t *urb, usbd_pipe_t *pipe, void *handle)
{
    int i = (int)(long)handle;

    (void)pipe;
    usbd_urb_status(urb, (uint32_t *)&in_status[i], (uint32_t *)&in_len[i]);
    __sync_synchronize();
    in_done[i] = 1;
}

static void out_complete(urb_t *urb, usbd_pipe_t *pipe, void *handle)
{
    (void)pipe;
    (void)handle;
    usbd_urb_status(urb, (uint32_t *)&out_status, (uint32_t *)&out_len);
    __sync_synchronize();
    out_done = 1;
}

static int wait_flag(volatile int *flag, int ms)
{
    int i;

    /* check before sleeping, then yield for a moment: usbd_io is asynchronous, and a
     * completion that has already landed costs nothing, but a 1ms sleep floor is a lot of
     * latency on a stream of 16 KB reads. the yield loop lets a completion that is about
     * to land run without waiting out a timer tick; a quiet link falls through to the
     * real sleeps. */
    for (i = 0; i < 16 && !*flag && !dead; i++) {
        sched_yield();
    }
    for (i = 0; i < ms && !*flag && !dead; i++) {
        usleep(1000);
    }
    return *flag;
}

/* ---- finding the gadget ------------------------------------------------------ */

static int find_vendor_iface(usbd_device_t *dev, uint32_t wanted, usbd_desc_node_t **ifn)
{
    usbd_desc_node_t *devn = 0, *cfgn = 0;
    usbd_descriptors_t *d;

    /* the descriptor tree is walked parent first: device, then its config, then the
     * config's interfaces, then each interface's endpoints */
    d = usbd_parse_descriptors(dev, 0, USB_DESC_DEVICE, 0, &devn);
    if (!d) {
        return -1;
    }
    d = usbd_parse_descriptors(dev, devn, USB_DESC_CONFIG, 0, &cfgn);
    if (!d) {
        return -1;
    }
    for (int i = 0; ; i++) {
        usbd_interface_descriptor_t *id;

        d = usbd_parse_descriptors(dev, cfgn, USB_DESC_INTERFACE, i, ifn);
        if (!d) {
            return -1;
        }
        id = (usbd_interface_descriptor_t *)d;
        if (id->bInterfaceNumber == wanted) {
            return (id->bInterfaceClass == USB_CLASS_VENDOR && id->bNumEndpoints == 2) ? 0 : -1;
        }
    }
}

static void insertion(usbd_connection_t *c, usbd_device_instance_t *inst)
{
    usbd_device_instance_t local;
    usbd_desc_node_t *ifn = 0, *epn = 0;
    usbd_descriptors_t *in_desc = 0, *out_desc = 0;
    usbd_descriptors_t *d;

    if (udev || dead) {
        return;
    }
    /* the gadget puts the vendor interface first, but try a few numbers so a gadget that
     * numbers its interfaces differently still works */
    for (int i = 0; i < 4; i++) {
        int rc;

        local = *inst;
        local.config = 1;
        local.iface = i;
        local.alternate = 0;
        rc = usbd_attach(c, &local, 0, &udev);
        if (rc != 0) {
            udev = 0;
            continue;
        }
        /* io-usb runs with -c, so config selection is the enumerator's job; ask for ours
         * (iofs-usb-ipod.so does the same) before reading the descriptor tree */
        usbd_select_config(udev, 1);
        if (find_vendor_iface(udev, i, &ifn) == 0) {
            break;
        }
        usbd_detach(udev);
        udev = 0;
    }
    if (!udev) {
        return;
    }
    /* index 0 of an interface's endpoints is the constructed control endpoint, the real
     * ones follow (devb-umass walks the same list) */
    for (int e = 0; e < 8; e++) {
        usbd_endpoint_descriptor_t *ed;

        d = usbd_parse_descriptors(udev, ifn, USB_DESC_ENDPOINT, e, &epn);
        if (!d) {
            break;
        }
        ed = (usbd_endpoint_descriptor_t *)d;
        if (ed->bmAttributes != USB_XFER_BULK) {
            continue;
        }
        if (ed->bEndpointAddress & 0x80) {
            in_desc = d;
        } else {
            out_desc = d;
        }
    }
    if (!in_desc || !out_desc) {
        goto fail;
    }
    if (usbd_open_pipe(udev, in_desc, &pipe_in) != 0) {
        printf("livi usb: open in pipe failed\n");
        pipe_in = 0;
        goto fail;
    }
    if (usbd_open_pipe(udev, out_desc, &pipe_out) != 0) {
        printf("livi usb: open out pipe failed\n");
        pipe_out = 0;
        goto fail;
    }
    buf_out = usbd_alloc(USB_TX_MAX);
    urb_out = usbd_alloc_urb(0);
    if (!buf_out || !urb_out) {
        goto fail;
    }
    for (int i = 0; i < USB_RX_URBS; i++) {
        buf_in[i] = usbd_alloc(USB_RX_MAX);
        urb_in[i] = usbd_alloc_urb(0);
        if (!buf_in[i] || !urb_in[i]) {
            goto fail;
        }
    }
    in_head = in_tail = in_flight = 0;
    dev_path = local.path;
    dev_no = local.devno;
    have_dev = 1;
    {
        /* the negotiated endpoint size says what the port is running at: 512 means the
         * usb83340 phy is at high speed, 64 means the stock force_fs. it is the number to
         * look at when the link is not behaving (see docs/homebrew.md, usbhs.sh) */
        int mps = ((usbd_endpoint_descriptor_t *)in_desc)->wMaxPacketSize;

        printf("livi usb gadget up (interface %d, bulk in %#x out %#x, %d-byte packets, "
               "%s)\n", (int)local.iface,
               ((usbd_endpoint_descriptor_t *)in_desc)->bEndpointAddress,
               ((usbd_endpoint_descriptor_t *)out_desc)->bEndpointAddress, mps,
               mps >= 512 ? "high speed" : "full speed");
    }
    return;

fail:
    if (pipe_in) {
        usbd_close_pipe(pipe_in);
        pipe_in = 0;
    }
    if (pipe_out) {
        usbd_close_pipe(pipe_out);
        pipe_out = 0;
    }
    if (udev) {
        usbd_detach(udev);
        udev = 0;
    }
}

static void removal(usbd_connection_t *c, usbd_device_instance_t *inst)
{
    (void)c;
    if (inst->path != dev_path || inst->devno != dev_no) {
        return;                     /* the mass storage interface, not ours */
    }
    dead = 1;
}

int usb_start(void)
{
    usbd_connect_parm_t parm;
    usbd_funcs_t funcs;
    usbd_device_ident_t ident;
    int tries, rc = 0;

    memset(&funcs, 0, sizeof funcs);
    funcs.nentries = _USBDI_NFUNCS;
    funcs.insertion = insertion;
    funcs.removal = removal;
    ident.vendor = ident.device = ident.dclass = ident.subclass = ident.protocol =
        USBD_CONNECT_WILDCARD;
    memset(&parm, 0, sizeof parm);
    parm.vusb = USB_VERSION;
    parm.vusbd = USBD_VERSION;
    parm.ident = &ident;
    parm.funcs = &funcs;

    for (tries = 0; tries < 50 && !conn; tries++) {
        rc = usbd_connect(&parm, &conn);
        if (rc != 0) {
            conn = 0;
            usleep(100000);         /* the usb manager may still be starting */
        }
    }
    if (!conn) {
        printf("livi usb: can't connect to the usb manager (%d)\n", rc);
        return -1;
    }
    for (tries = 0; tries < 150 && !have_dev && !dead; tries++) {
        usleep(100000);
    }
    if (!have_dev) {
        printf("livi usb: no livi gadget after %d s\n", tries / 10);
        return -1;
    }
    return 0;
}

/* ---- the bulk pipes ---------------------------------------------------------- */

int usb_read(void *dst, int n, int timeout_ms)
{
    int len;

    if (!have_dev || dead) {
        return -1;
    }
    if (n > USB_RX_MAX) {
        n = USB_RX_MAX;
    }
    /* keep the ring full, so the next 16 KB is already in flight while this one is being
     * consumed. each urb asks for the full 16 KB, so a caller has to take at least that
     * much (the reader's rx_fill does). */
    while (in_flight < USB_RX_URBS && !dead) {
        int i = in_tail;

        in_done[i] = 0;
        if (usbd_setup_bulk(urb_in[i], URB_DIR_IN | URB_SHORT_XFER_OK, buf_in[i],
                            USB_RX_MAX) != 0 ||
            usbd_io(urb_in[i], pipe_in, in_complete, (void *)(long)i, 0) != 0) {
            dead = 1;
            return -1;
        }
        in_tail = (in_tail + 1) % USB_RX_URBS;
        in_flight++;
    }
    if (!wait_flag(&in_done[in_head], timeout_ms + 100)) {
        /* the completion never came. abort the pipe to force one out of the stack, and
         * treat a second miss as a dead link rather than a lost wakeup */
        usbd_abort_pipe(pipe_in);
        if (!wait_flag(&in_done[in_head], 500)) {
            dead = 1;
            return -1;
        }
    }
    if (dead) {
        return -1;
    }
    if ((in_status[in_head] & 0xff000000u) != USBD_STATUS_CMP) {
        /* timeout or abort: a quiet link, not a dead one. the urb is consumed all the
         * same, so the ring keeps moving */
        in_done[in_head] = 0;
        in_flight--;
        in_head = (in_head + 1) % USB_RX_URBS;
        return 0;
    }
    len = (int)in_len[in_head];
    if (len > n) {
        len = n;
    }
    memcpy(dst, buf_in[in_head], len);
    in_done[in_head] = 0;
    in_flight--;
    in_head = (in_head + 1) % USB_RX_URBS;
    return len;
}

int usb_write(const void *src, int n)
{
    const unsigned char *p = src;

    if (!have_dev || dead) {
        return -1;
    }
    while (n > 0) {
        int chunk = n > USB_TX_MAX ? USB_TX_MAX : n;

        memcpy(buf_out, p, chunk);
        out_done = 0;
        if (usbd_setup_bulk(urb_out, URB_DIR_OUT, buf_out, chunk) != 0 ||
            usbd_io(urb_out, pipe_out, out_complete, 0, USB_TIMEOUT_MS) != 0) {
            dead = 1;
            return -1;
        }
        if (!wait_flag(&out_done, USB_TIMEOUT_MS + 100) ||
            (out_status & 0xff000000u) != USBD_STATUS_CMP) {
            return -1;
        }
        p += chunk;
        n -= chunk;
    }
    return 0;
}
