/*
 * rawplay's usb transport: the vendor interface of the composite livi gadget, with a
 * bulk-in path that can point its urbs straight at the caller's memory (the panel
 * mapping) so the usb controller's dma writes the frame where it is scanned out.
 *
 * the bulk machinery follows common/livi-usb.c (same descriptor walk, same urb ring shape),
 * but the read side is built around a raw byte stream rather than fixed 16 KB copies:
 *
 *   - direct mode: up to RAW_URBS urbs are in flight, each writing at its own offset of
 *     the destination. completions are consumed in submission order; a completion that
 *     came back short is compacted down to where the stream actually continues, so the
 *     bytes always land in stream order. when the gadget delivers request-sized
 *     transfers (functionfs on real hardware, or qemu's usb-livi chunked=on) the
 *     memmove never runs and the frame is truly zero-copy.
 *   - staged mode: one usbd_alloc buffer at a time, copied out after each completion.
 *     this is the guaranteed path and the announced fallback when direct setup fails.
 *
 * a read never aborts: this stack can segfault the completion thread when an aborted
 * bulk-in urb is handed back, so a quiet link is waited out (the removal callback turns
 * an unplug into dead instead). every submission nevertheless carries a generation tag,
 * so a completion that races a slot's reuse is ignored rather than corrupting a read.
 */
#include "qnx.h"
#include "raw-usb.h"
#include "usbdi.h"
#include "jlog.h"

#define RAW_CHUNK       262144
/* the direct path submits urbs straight at the destination, so its chunk is only bounded
 * by the destination (the panel mapping) and the gadget's buffering: a whole frame in one
 * urb is legal and costs one completion instead of three. the staging ring stays at
 * RAW_CHUNK because each slot is an allocated buffer. */
#define RAW_DIRECT_MAX  (1024 * 1024)
#define RAW_URBS        4
#define RAW_TX_MAX      512
#define RAW_TIMEOUT_MS  200
#define USB_CLASS_VENDOR 0xff
#define USB_XFER_BULK   2

static usbd_connection_t *conn;
static usbd_device_t *udev;
static usbd_pipe_t *pipe_in, *pipe_out;
static urb_t *urb_in[RAW_URBS];
static urb_t *urb_out;
static unsigned char *stage[RAW_URBS], *scratch, *buf_out;
static volatile int have_dev, dead;
static int dev_path = -1, dev_no = -1;
static int in_mps = 512;

static volatile int rx_done[RAW_URBS], out_done;
static volatile uint32_t rx_status[RAW_URBS], rx_len[RAW_URBS], out_status, out_len;
static volatile int rx_gen[RAW_URBS];
static unsigned char *rx_buf[RAW_URBS];
static int rx_start[RAW_URBS];
static int rx_head, rx_tail, rx_inflight;
static volatile int rx_error;   /* a completion came back with a transfer error */
static int rx_busy[RAW_URBS];   /* submitted and not yet consumed */
static int rx_drop[RAW_URBS];   /* belongs to an abandoned frame: consume silently */
static int rx_epoch[RAW_URBS];  /* which read submitted it (see abandon_read) */
static int cur_epoch;

static int direct_mode, direct_broken, compacted;
static int shorts;

/* the read wait is bounded: a link that never delivers a byte must not sit the player in
 * an uninterruptible wait (a real unit has no console and the hmi is stopped behind it).
 * quiet_out distinguishes "nothing arrived inside the timeout" from a removed device, so
 * the caller can say which happened; stop is the exit gesture, and is the only way out of
 * a wait on a link that is neither dead nor timed out yet. */
static volatile int stop;
static int quiet_out;
static long long last_activity;
static int errs, logged_first;

/* the bulk-in urb size. 256 KB was tuned against qemu, but a real unit's ehci driver
 * behaves differently (the mpeg path there has always used 16 KB), so it is a runtime
 * knob: --urb on the player. must be a multiple of the maxpacket. the staging ring slots
 * are RAW_CHUNK each; a bigger urb only changes how the direct path splits the frame. */
static int raw_chunk = RAW_CHUNK;

void raw_usb_set_chunk(int bytes)
{
    if (bytes < 512) {
        bytes = 512;
    }
    if (bytes > RAW_DIRECT_MAX) {
        bytes = RAW_DIRECT_MAX;
    }
    raw_chunk = bytes & ~511;
}

int raw_usb_chunk(void)
{
    return raw_chunk;
}

static long long now_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* the completion thread wakes the reader through a semaphore instead of a polled flag:
 * polling costs a full timer tick (a millisecond) per urb, and a raw frame is only three
 * of them. the timeout form is only used so a removed device (dead) is noticed. */
typedef union {
    long long _align;
    unsigned char _pad[64];
} raw_sem_t;

int sem_init(raw_sem_t *sem, int pshared, unsigned value);
int sem_post(raw_sem_t *sem);
int sem_timedwait(raw_sem_t *sem, const struct timespec *abs);

static raw_sem_t rx_sem, out_sem;

static void in_complete(urb_t *urb, usbd_pipe_t *pipe, void *handle)
{
    long h = (long)handle;
    int i = (int)(h & 0xff), gen = (int)(h >> 8);

    (void)pipe;
    if (rx_gen[i] != gen) {
        return;                     /* a late completion from a dropped urb */
    }
    usbd_urb_status(urb, (uint32_t *)&rx_status[i], (uint32_t *)&rx_len[i]);
    __sync_synchronize();
    rx_done[i] = 1;
    sem_post(&rx_sem);
}

static void out_complete(urb_t *urb, usbd_pipe_t *pipe, void *handle)
{
    (void)pipe;
    (void)handle;
    usbd_urb_status(urb, (uint32_t *)&out_status, (uint32_t *)&out_len);
    __sync_synchronize();
    out_done = 1;
    sem_post(&out_sem);
}

static void sem_init_all(void)
{
    sem_init(&rx_sem, 0, 0);
    sem_init(&out_sem, 0, 0);
}

/* returns 1 when the flag is set, 0 on timeout, a dead link or a stop */
static int wait_flag(volatile int *flag, int ms)
{
    struct timespec ts;

    for (;;) {
        if (*flag || dead || stop) {
            return *flag;
        }
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += (long)ms * 1000000;
        while (ts.tv_nsec >= 1000000000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
        }
        sem_timedwait(flag == &out_done ? &out_sem : &rx_sem, &ts);
        if (*flag || dead || stop) {
            return *flag;
        }
        if (ms <= 100) {
            return 0;               /* the caller's own poll interval, used by dead checks */
        }
    }
}

/* ---- finding the gadget, the same walk livi-usb.c does ------------------------ */

static int find_vendor_iface(usbd_device_t *dev, uint32_t wanted, usbd_desc_node_t **ifn)
{
    usbd_desc_node_t *devn = 0, *cfgn = 0;
    usbd_descriptors_t *d;

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

static int open_pipes(usbd_descriptors_t *in_desc, usbd_descriptors_t *out_desc)
{
    if (usbd_open_pipe(udev, in_desc, &pipe_in) != 0) {
        pipe_in = 0;
        return -1;
    }
    if (usbd_open_pipe(udev, out_desc, &pipe_out) != 0) {
        usbd_close_pipe(pipe_in);
        pipe_in = 0;
        pipe_out = 0;
        return -1;
    }
    return 0;
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
        /* do not select the configuration here. the composite device is normally
         * already configured: devb-umass attached the mass storage interface and
         * selected config 1 at enumeration. asking for it again makes functionfs
         * disable and re-enable every function (the gadget's ffs sees ESHUTDOWN), and
         * on this stack the bulk-in pipe can come back unserviced: rawplay then waits
         * for a byte that never arrives while the host side's writes stay blocked.
         * parse the tree and open the pipes on the configuration already active; only
         * a genuinely unconfigured device gets the explicit select below. */
        if (find_vendor_iface(udev, i, &ifn) == 0) {
            break;
        }
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
    if (open_pipes(in_desc, out_desc) != 0) {
        /* no configuration active yet (nothing else attached the device): select the
         * one there is and give the gadget a moment to enable its endpoints, then open
         * again. the settle covers functionfs's enable landing after the control
         * transfer's status stage. */
        usbd_select_config(udev, 1);
        usleep(200000);
        if (open_pipes(in_desc, out_desc) != 0) {
            printf("rawplay: usb: open pipes failed\n");
            goto fail;
        }
    }
    buf_out = usbd_alloc(RAW_TX_MAX);
    urb_out = usbd_alloc_urb(0);
    scratch = usbd_alloc(RAW_CHUNK);
    for (int i = 0; i < RAW_URBS; i++) {
        stage[i] = usbd_alloc(RAW_CHUNK);
        urb_in[i] = usbd_alloc_urb(0);
        if (!stage[i] || !urb_in[i]) {
            goto fail;
        }
    }
    if (!buf_out || !urb_out || !scratch) {
        goto fail;
    }
    rx_head = rx_tail = rx_inflight = 0;
    sem_init_all();
    dev_path = local.path;
    dev_no = local.devno;
    have_dev = 1;
    in_mps = ((usbd_endpoint_descriptor_t *)in_desc)->wMaxPacketSize & 0x7ff;
    {
        int mps = ((usbd_endpoint_descriptor_t *)in_desc)->wMaxPacketSize;

        printf("rawplay: usb gadget up (bulk in %#x out %#x, %d-byte packets, %s)\n",
               ((usbd_endpoint_descriptor_t *)in_desc)->bEndpointAddress,
               ((usbd_endpoint_descriptor_t *)out_desc)->bEndpointAddress, mps,
               mps >= 512 ? "high speed" : "full speed");
        jlog("rawplay: usb gadget up (in %#x out %#x, %d-byte packets, %s)",
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
        return;
    }
    dead = 1;
}

int raw_usb_start(void)
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

    for (tries = 0; tries < 300 && !conn; tries++) {
        rc = usbd_connect(&parm, &conn);
        if (rc != 0) {
            conn = 0;
            usleep(100000);
        }
    }
    if (!conn) {
        printf("rawplay: usb: can't connect to the usb manager (%d)\n", rc);
        return -1;
    }
    for (tries = 0; tries < 200 && !have_dev && !dead; tries++) {
        usleep(100000);
    }
    if (!have_dev) {
        printf("rawplay: usb: no livi gadget after %d s\n", tries / 10);
        jlog("rawplay: usb: no livi gadget after %d s", tries / 10);
        return -1;
    }
    last_activity = now_us();
    return 0;
}

/* ---- the read side ------------------------------------------------------------ */

void raw_usb_set_direct(int on)
{
    direct_mode = on && !direct_broken;
}

int raw_usb_direct(void)
{
    return direct_mode && !direct_broken;
}

int raw_usb_compacted(void)
{
    return compacted;
}

static int submit_in(int slot, void *buf, int want, int start)
{
    int gen = rx_gen[slot] + 1;

    rx_buf[slot] = buf;
    rx_start[slot] = start;
    rx_done[slot] = 0;
    rx_drop[slot] = 0;
    rx_epoch[slot] = cur_epoch;
    rx_gen[slot] = gen;
    if (usbd_setup_bulk(urb_in[slot], URB_DIR_IN | URB_SHORT_XFER_OK, buf, want) != 0 ||
        usbd_io(urb_in[slot], pipe_in, in_complete, (void *)(long)(slot | gen << 8), 0) != 0) {
        jlog("rawplay: bulk-in submit failed (want %d)", want);
        return -1;
    }
    rx_busy[slot] = 1;
    rx_tail = (rx_tail + 1) % RAW_URBS;
    rx_inflight++;
    return 0;
}

/* a read is giving up (a bulk-in error landed in it). the urbs it still has in flight
 * hold bytes of the frame it was reading: when they complete, their offsets no longer
 * match what the next read expects, so they are tagged to be consumed without copying
 * (wait_head skips a drop). without this the recovery either drained them for up to two
 * seconds or, worse, a late completion would be copied to the wrong place. */
static void abandon_read(void)
{
    int i;

    for (i = 0; i < RAW_URBS; i++) {
        if (rx_busy[i] && rx_epoch[i] == cur_epoch) {
            rx_drop[i] = 1;
        }
    }
}

/* wait for the oldest completion, but not forever. this stack does not safely complete an
 * aborted bulk-in urb (a timeout followed by usbd_abort_pipe segfaults the completion
 * thread), so a read never aborts: it waits until the gadget answers, the device goes
 * away (dead), the caller asks it to stop (the exit gesture) or timeout_ms passes with
 * no byte at all. returns the completion length, or -1 (raw_usb_quiet() says why). */
static int wait_head(int timeout_ms)
{
    for (;;) {
        int slot = rx_head;
        int len, drop;

        if (rx_inflight == 0) {
            return 0;               /* nothing outstanding */
        }
        for (;;) {
            if (!wait_flag(&rx_done[slot], 100)) {
                if (dead || stop) {
                    return -1;
                }
                if (timeout_ms > 0 &&
                    now_us() - last_activity > (long long)timeout_ms * 1000) {
                    quiet_out = 1;
                    return -1;
                }
                continue;
            }
            break;
        }
        drop = rx_drop[slot];
        rx_busy[slot] = 0;
        rx_done[slot] = 0;
        rx_inflight--;
        rx_head = (rx_head + 1) % RAW_URBS;
        if ((rx_status[slot] & 0xff000000u) != USBD_STATUS_CMP) {
            /* a bulk-in error (a stall from a pipe opened across a functionfs re-enable is
             * the one seen on a real car): the transfer is gone, and the endpoint stays
             * halted until the pipe is reset, so retrying without it spins forever. only
             * reset when nothing else is in flight, or the reset would abort urbs the
             * completion thread may never hand back (this stack segfaults on that). */
            uint32_t st = rx_status[slot];

            rx_error = 1;
            if (++errs == 2) {
                jlog("rawplay: bulk-in error status %#x, resetting the pipe", st);
                printf("rawplay: bulk-in error status %#x, resetting the pipe\n", st);
            }
            if (errs >= 2 && rx_inflight == 0) {
                errs = 0;
                usbd_reset_pipe(pipe_in);
            }
            return -3;              /* the caller abandons the read and resyncs */
        }
        if (drop) {
            continue;               /* an abandoned frame's bytes: consume and look on */
        }
        len = (int)rx_len[slot];
        if (len <= 0) {
            continue;
        }
        if (!logged_first) {
            logged_first = 1;
            printf("rawplay: first %d bytes (%02x %02x %02x %02x)\n", len,
                   rx_buf[slot][0], len > 1 ? rx_buf[slot][1] : 0,
                   len > 2 ? rx_buf[slot][2] : 0, len > 3 ? rx_buf[slot][3] : 0);
            jlog("rawplay: first %d bytes (%02x %02x %02x %02x)", len,
                 rx_buf[slot][0], len > 1 ? rx_buf[slot][1] : 0,
                 len > 2 ? rx_buf[slot][2] : 0, len > 3 ? rx_buf[slot][3] : 0);
        }
        errs = 0;
        last_activity = now_us();
        quiet_out = 0;
        return len;
    }
}

static int read_direct(void *dst, int n, int timeout_ms)
{
    int done = 0, submitted = 0;

    cur_epoch++;
    while (done < n) {
        while (rx_inflight < RAW_URBS && submitted < n) {
            int want = n - submitted > raw_chunk ? raw_chunk : n - submitted;

            if (submit_in(rx_tail, (char *)dst + submitted, want, submitted) != 0) {
                /* with nothing in flight yet this is the stack refusing a caller
                 * buffer and the caller can still fall back; mid-read it is dead. */
                if (rx_inflight == 0 && done == 0) {
                    direct_broken = 1;
                    return -2;
                }
                dead = 1;
                return -1;
            }
            submitted += want;
        }
        if (rx_inflight == 0) {
            /* everything asked for is gone (consumed as abandoned bytes) and no byte of
             * it arrived: there is nothing left to wait for */
            return -1;
        }
        {
            int slot = rx_head;
            int len = wait_head(timeout_ms);

            if (len == -3) {
                /* a transfer error: the bytes are gone mid-read and there is no way to
                 * know how much of the destination is valid, so the read fails and the
                 * caller resyncs on the next header (raw_usb_resync, raw_usb_error). the
                 * urbs still in flight are tagged so their late completions cannot land
                 * in whatever is read next. */
                abandon_read();
                return -1;
            }
            if (len < 0) {
                return -1;
            }
            if (len > 0) {
                if (rx_start[slot] != done) {
                    memmove((char *)dst + done, rx_buf[slot], len);
                    compacted = 1;
                    shorts++;
                }
                done += len;
            }
        }
    }
    /* repeated short completions mean the transport is not the request-sized one the host
     * declared (qemu's usb-livi without chunked=on, or a peer writing in pieces). the
     * current read is finished correctly by compaction, then the staging ring takes over:
     * a plain copy per completion beats shuffling a landing zone that never lines up. */
    if (shorts >= 2 && !direct_broken) {
        printf("rawplay: short bulk transfers (%d), using the staging ring from here\n", shorts);
        direct_broken = 1;
    }
    return done;
}

static int read_staged(void *dst, int n, int timeout_ms)
{
    int done = 0;
    int maxw = raw_chunk < RAW_CHUNK ? raw_chunk : RAW_CHUNK;

    cur_epoch++;
    while (done < n) {
        int want = n - done > maxw ? maxw : n - done;
        int slot;
        int len;

        if (submit_in(rx_tail, stage[rx_tail], want, done) != 0) {
            dead = 1;
            return -1;
        }
        slot = rx_head;
        len = wait_head(timeout_ms);
        if (len == -3) {
            abandon_read();
            return -1;              /* error: the caller resyncs */
        }
        if (len < 0) {
            return -1;
        }
        if (len > n - done) {
            len = n - done;             /* a carried-over urb can hold more than asked */
        }
        memcpy((char *)dst + done, rx_buf[slot], len);
        done += len;
    }
    return done;
}

int raw_usb_read(void *dst, int n, int timeout_ms)
{
    int got = 0;

    while (got < n && !dead && !stop) {
        int r;

        if (direct_mode && !direct_broken) {
            r = read_direct((char *)dst + got, n - got, timeout_ms);
            if (r == -2) {
                direct_broken = 1;
                continue;
            }
        } else {
            r = read_staged((char *)dst + got, n - got, timeout_ms);
        }
        if (r < 0) {
            printf("rawplay: usb read failed (direct=%d broken=%d dead=%d quiet=%d)\n",
                   direct_mode, direct_broken, dead, quiet_out);
            jlog("rawplay: usb read failed (direct=%d broken=%d dead=%d quiet=%d, "
                 "got %d/%d)", direct_mode, direct_broken, dead, quiet_out, got, n);
            return -1;
        }
        got += r;
    }
    return (dead || stop) ? -1 : got;
}

void raw_usb_stop(void)
{
    stop = 1;
}

int raw_usb_quiet(void)
{
    return quiet_out;
}

/* 1 when a bulk-in completion came back with a transfer error since the last resync. the
 * bytes of the transfer are lost, so the caller must abandon the current frame and let
 * the header scanner find the next one (the stream is framed, there is no in-band
 * resync once a read is short). */
int raw_usb_error(void)
{
    return rx_error;
}

/* after a bulk-in error the caller abandons the current frame and calls this. the urbs of
 * that read are already tagged (abandon_read), so there is nothing to drain: the next read
 * consumes their completions silently, in ring order. the old version waited for the
 * endpoint to hand every one of them back before returning, which under load took seconds
 * and cost more throughput than the error itself. a halted pipe still gets its reset from
 * wait_head, once nothing is in flight. */
void raw_usb_resync(void)
{
    rx_error = 0;
}

int raw_usb_mps(void)
{
    return in_mps;
}

void *raw_usb_scratch(void)
{
    return scratch;
}

int raw_usb_scratch_size(void)
{
    return RAW_CHUNK;
}

/* ---- the write side, the same shape as livi-usb.c ------------------------------ */

int raw_usb_write(const void *src, int n)
{
    const unsigned char *p = src;

    if (!have_dev || dead) {
        return -1;
    }
    while (n > 0) {
        int chunk = n > RAW_TX_MAX ? RAW_TX_MAX : n;

        memcpy(buf_out, p, chunk);
        out_done = 0;
        if (usbd_setup_bulk(urb_out, URB_DIR_OUT, buf_out, chunk) != 0 ||
            usbd_io(urb_out, pipe_out, out_complete, 0, RAW_TIMEOUT_MS) != 0) {
            dead = 1;
            return -1;
        }
        if (!wait_flag(&out_done, RAW_TIMEOUT_MS + 100) ||
            (out_status & 0xff000000u) != USBD_STATUS_CMP) {
            return -1;
        }
        p += chunk;
        n -= chunk;
    }
    return 0;
}
