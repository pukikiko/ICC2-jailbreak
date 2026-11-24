/*
 * rawplay - the unit end of the raw livi link. it receives uncompressed rgb565 frames on
 * the composite gadget's vendor interface and puts them into the graphics surface it owns
 * above the hmi (or into the panel framebuffer when it has to stop the hmi), in the panel's
 * own format, with no decode, no colour conversion and no scaling. touch/knob/button go
 * back on the same wire. the host end is rawplay/rawlink.c, the protocol is in
 * rawplay/README.md section 2.2.
 *
 * the bulk-in urbs point straight at that surface mapping, so the usb controller's dma
 * writes each frame where the panel scans it out (zero copy). if the stack will not take
 * the buffer, the transport falls back to a dma staging buffer and one copy, which is
 * printed at startup and shown in --stats; it is never silent.
 *
 * usage: rawplay --usb [seconds] [--stats] [--stage] [--direct] [--hmi]
 */
#include "qnx.h"
#include "fb.h"
#include "console.h"
#include "input.h"
#include "raw-usb.h"
#include "hmictl.h"
#include "jlog.h"
#include "lr-audio.h"
#include "qnx-audio.h"

/* the video is drawn by a small child process that owns the spare overlay layer. the
 * hmi keeps running and drawing into the panel's framebuffer; the parent rawplay writes
 * frames straight into the surface physical address the child publishes. when an hmi
 * window opens the child is killed (io-display disables the layer on client death), and
 * when the window closes a fresh child takes the layer again. a fresh process is the
 * reliable way back: reattaching inside the long-lived player after the hmi has used the
 * display leaves the layer hidden. if the child cannot take the layer the player falls
 * back to the old SIGSTOP/SIGCONT session behaviour. */
int gf_dev_attach(void **dev, unsigned device, int *err);
int gf_dev_detach(void *dev);
int gf_display_attach(void **disp, void *dev, unsigned display, int *err);
int gf_display_detach(void *disp);
int gf_layer_attach(void **layer, void *disp, unsigned index, int flags);
int gf_layer_detach(void *layer);
int gf_layer_enable(void *layer);
int gf_layer_update(void *layer, unsigned flags);
int gf_layer_set_surfaces(void *layer, void *surfaces[], int nsurfs);
int gf_layer_set_dst_viewport(void *layer, int x1, int y1, int x2, int y2);
int gf_surface_create_layer(void **psurface, void *layers[], int nlayers,
                            int surface_index, int w, int h, unsigned format,
                            const void *palette, unsigned flags);
int gf_surface_get_info(void *surface, void *info);
int gf_surface_free(void *surface);

#define GF_FORMAT_RGB565_LE 0x1110
#define VIDEO_LAYER         1
#define LAYER_STATE         "/tmp/rawplay-layer.state"

extern char **environ;
pid_t getppid(void);

static pid_t layer_pid;
static pixel *fb_direct;        /* the hmi's real framebuffer, kept for the fallback */
static pixel *fb_layer;         /* the child's surface, where frames are streamed */
static unsigned long long layer_paddr;
static int layer_video;
static unsigned gf_info[16];
static int opt_hmi;
static struct fb fb;
static const char *self_path;
static char layer_state_path[64];

static unsigned long now_ms(void);

/* the layer helper publishes its surface asynchronously. a window close starts it and
 * keeps reading (audio) until the state file appears, so the climate/reverse window no
 * longer pauses the whole player while io-display hands the layer back. */
#define LAYER_ATTACH_TIMEOUT_MS 2000

static void layer_helper_stop(void)
{
    if (layer_pid > 0) {
        kill(layer_pid, SIGKILL);
        waitpid(layer_pid, 0, 0);
        layer_pid = 0;
    }
}

/* spawn the helper but do not wait for the layer: the caller polls layer_attach() */
static int layer_start(void)
{
    char wbuf[16], hbuf[16], pidbuf[16];
    char *argv[7];

    unlink(layer_state_path);
    snprintf(wbuf, sizeof wbuf, "%u", fb.width);
    snprintf(hbuf, sizeof hbuf, "%u", fb.height);
    argv[0] = (char *)self_path;
    argv[1] = (char *)"--layer-helper";
    argv[2] = layer_state_path;
    argv[3] = wbuf;
    argv[4] = hbuf;
    argv[5] = pidbuf;           /* parent pid */
    argv[6] = 0;
    snprintf(pidbuf, sizeof pidbuf, "%d", (int)getpid());
    if (posix_spawn(&layer_pid, self_path, 0, 0, argv, environ) != 0) {
        jlog("rawplay: cannot start the layer helper (%s)", self_path);
        layer_pid = 0;
        return -1;
    }
    return 0;
}

/* 1: the surface is mapped and frames are going there, 0: not published yet,
 * -1: the helper is gone or its state file is unusable. one short read per call, so the
 * main loop can keep draining the link (and the PCM chunks in it) while it waits. */
static int layer_attach(void)
{
    char ebuf[64];
    void *p;
    unsigned long long paddr = 0;
    unsigned stride = 0;
    int fd, n;

    fd = open(layer_state_path, O_RDONLY);
    if (fd >= 0) {
        n = read(fd, ebuf, sizeof ebuf - 1);
        close(fd);
        if (n > 0) {
            char *e;

            ebuf[n] = 0;
            paddr = (unsigned long long)strtoul(ebuf, &e, 0);
            paddr |= (unsigned long long)strtoul(e, &e, 0) << 32;
            stride = (unsigned)strtoul(e, &e, 0);
        }
    }
    if (!paddr || stride != fb.stride) {
        return layer_pid > 0 ? 0 : -1;
    }
    unlink(layer_state_path);
    p = mmap_device_memory(0, fb.stride * fb.height,
                           PROT_READ | PROT_WRITE | PROT_NOCACHE, 0, paddr);
    if (p == MAP_FAILED) {
        jlog("rawplay: cannot map the layer helper surface");
        return -1;
    }
    if (fb_layer) {
        munmap(fb_layer, fb.stride * fb.height);
    }
    fb_layer = p;
    fb.pixels = p;
    layer_paddr = paddr;
    return 1;
}

/* the startup/open path: spawn and wait for the layer before the stream starts */
static int layer_spawn(void)
{
    if (layer_start() != 0) {
        return -1;
    }
    for (int i = 0; i < LAYER_ATTACH_TIMEOUT_MS / 50; i++) {
        int r = layer_attach();

        if (r == 1) {
            return 0;
        }
        if (r < 0) {
            break;
        }
        usleep(50000);
    }
    jlog("rawplay: layer helper did not publish a usable surface");
    layer_helper_stop();
    unlink(layer_state_path);
    return -1;
}

static void panel_layer_close(void)
{
    layer_helper_stop();
    if (fb_layer) {
        munmap(fb_layer, fb.stride * fb.height);
        fb_layer = 0;
    }
    if (fb_direct) {
        fb.pixels = fb_direct;
    }
    layer_video = 0;
}

/* hand the panel back: drop the video layer, or resume the hmi in the fallback path */
static void panel_finish(void)
{
    if (layer_video) {
        panel_layer_close();
    } else if (opt_hmi) {
        hmi_signal(HMI_SIGCONT);
    }
}

static int panel_layer_open(void)
{
    if (!opt_hmi) {
        return -1;
    }
    fb_direct = fb.pixels;
    snprintf(layer_state_path, sizeof layer_state_path, "/tmp/rawplay-layer-%d",
             (int)getpid());
    if (layer_spawn() != 0) {
        return -1;
    }
    layer_video = 1;
    printf("rawplay: video on layer %d, surface %#llx\n", VIDEO_LAYER, layer_paddr);
    jlog("rawplay: video on layer %d, surface %#llx", VIDEO_LAYER, layer_paddr);
    return 0;
}

/* drop the layer so the hmi underneath is visible; the child death makes io-display
 * disable layer 1 */
static void panel_layer_hide(void)
{
    if (!layer_video) {
        return;
    }
    layer_helper_stop();
    if (fb_layer) {
        munmap(fb_layer, fb.stride * fb.height);
        fb_layer = 0;
    }
    if (fb_direct) {
        fb.pixels = fb_direct;
    }
}

/* the return path after a window: spawn the helper and let the main loop keep reading
 * until layer_attach() sees the new surface. this is what keeps the PCM stream fed while
 * the climate bar is closing (a blocking io-display attach used to silence it). */
static unsigned long layer_resume_ms;

static int panel_layer_resume_start(void)
{
    if (!layer_video) {
        return -1;
    }
    panel_layer_hide();         /* the window already dropped it; this is a no-op */
    unlink(layer_state_path);
    if (layer_start() != 0) {
        return -1;
    }
    layer_resume_ms = now_ms();
    return 0;
}

/* 1: attached, 0: still waiting, -1: failed or timed out */
static int panel_layer_resume_poll(void)
{
    int r = layer_attach();

    if (r == 1) {
        return 1;
    }
    if (r < 0 || (long)(now_ms() - layer_resume_ms) >= LAYER_ATTACH_TIMEOUT_MS) {
        return -1;
    }
    return 0;
}

static void panel_layer_resume_abort(void)
{
    layer_helper_stop();
    if (fb_layer) {
        munmap(fb_layer, fb.stride * fb.height);
        fb_layer = 0;
    }
    if (fb_direct) {
        fb.pixels = fb_direct;
    }
    unlink(layer_state_path);
}

/* The helper owns layer 1 and the surface the parent streams into. It writes the surface
 * physical address to the state file, then just keeps the layer alive. */
static int layer_helper(const char *state, unsigned w, unsigned h, pid_t parent)
{
    void *dev = 0, *disp = 0, *layer = 0, *surface = 0;
    void *layers[1];
    unsigned long long paddr;
    unsigned stride;
    char buf[64];
    int fd, n;
    pid_t ppid = getppid();

    if (gf_dev_attach(&dev, 0, 0) != 0 || !dev) {
        return 1;
    }
    if (gf_display_attach(&disp, dev, 0, 0) != 0 || !disp) {
        gf_dev_detach(dev);
        return 1;
    }
    if (gf_layer_attach(&layer, disp, VIDEO_LAYER, 0) != 0 || !layer) {
        gf_display_detach(disp);
        gf_dev_detach(dev);
        return 1;
    }
    layers[0] = layer;
    if (gf_surface_create_layer(&surface, layers, 1, 0, w, h,
                                GF_FORMAT_RGB565_LE, 0, 0) != 0 || !surface) {
        gf_layer_detach(layer);
        gf_display_detach(disp);
        gf_dev_detach(dev);
        return 1;
    }
    memset(gf_info, 0, sizeof gf_info);
    gf_surface_get_info(surface, gf_info);
    stride = gf_info[4];
    paddr = (unsigned long long)gf_info[6] | (unsigned long long)gf_info[7] << 32;
    gf_layer_set_surfaces(layer, &surface, 1);
    gf_layer_set_dst_viewport(layer, 0, 0, w, h);
    gf_layer_enable(layer);
    if (paddr) {
        /* leave the surface black instead of whatever the allocator hands back; the
         * first video frame replaces it */
        unsigned short *p = mmap_device_memory(0, stride * h,
                                               PROT_READ | PROT_WRITE | PROT_NOCACHE,
                                               0, paddr);
        if (p != MAP_FAILED) {
            for (unsigned i = 0; i < stride * h / 2; i++) p[i] = 0;
            munmap(p, stride * h);
        }
    }
    if (!paddr || gf_layer_update(layer, 0) != 0) {
        gf_surface_free(surface);
        gf_layer_detach(layer);
        gf_display_detach(disp);
        gf_dev_detach(dev);
        return 1;
    }
    fd = open(state, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        gf_surface_free(surface);
        gf_layer_detach(layer);
        gf_display_detach(disp);
        gf_dev_detach(dev);
        return 1;
    }
    n = snprintf(buf, sizeof buf, "%u %u %u\n", (unsigned)paddr,
                 (unsigned)(paddr >> 32), stride);
    write(fd, buf, n);
    close(fd);
    for (;;) {
        usleep(500000);
        if (getppid() != ppid || ppid != parent) {
            return 0;
        }
    }
}

#define LR_MAGIC0    'L'
#define LR_MAGIC1    'R'
#define LR_HEADER    16
#define LR_MSG       512    /* every LR message is padded to one max packet (see below) */
#define LR_MODE      1
#define LR_FRAME     2
#define FMT_RGB565   1
#define MODE_PAYLOAD 16

#define LI_TOUCH     1
#define LI_KNOB      2
#define LI_BUTTON    3
#define LI_READY     5
#define LI_RAW       8
/* the stream watchdog, the same shape the serial players used: a live link that goes
 * quiet exits, a link that never delivered anything waits longer for the host to start. */
#define STARTUP_DEAD_US 15000000
#define LINK_DEAD_US     5000000
#define READY_INTERVAL_US 1000000

/* the climate panel keys (their bits on ipc channel 6, see ICC2 sdk input.c). the hvac state
 * changes on the v850's own side when one is pressed, but the only place that state is
 * shown is the hmi's climate bar, and the video overlay covers it. so a press opens a
 * window with --hmi: streaming pauses, the overlay is dropped so the hmi's bar shows, and
 * a fresh press keeps the window open. a held key holds it open too. */
#define HMI_WINDOW_MS    1000
#define HMI_WINDOW_STR_(x) #x
#define HMI_WINDOW_STR(x)  HMI_WINDOW_STR_(x)
static const unsigned char climate_keys[] = { 6, 7, 8, 9, 11, 12, 13, 14, 16, 17, 18, 19, 20 };

/* the rest of the car that has to be shown: vehicle.state (ipc ch 4, msg 0 v7) says when
 * the gear is reverse. reverse gear is a state, not an edge, so it holds the window open
 * from the gear engaging until it leaves reverse. day/night is different: the HMI runs in
 * the background and themes itself while the video covers it, so no window is needed.
 * the reader asks for the current vehicle state when it connects, like hmictl.c's car
 * guard, so a player started while reverse is already engaged does not miss it. */
#define IPC_MONITOR      "/dev/ipc/0"
#define VEHICLE_CH       4
#define VSTATE_MSG       0x00
#define VSTATE_VER       7
#define VSTATE_GEAR      1      /* data byte 1 */
#define VSTATE_LEN       7      /* msg id + version + 5 data bytes */
#define GEAR_REVERSE     11
#define VEHICLE_SILENT   500    /* 5 s of quiet reads reopens the monitor, like input.c */

/* what last extended the window, for the log only */
#define HMI_FROM_CLIMATE 0
#define HMI_FROM_REVERSE 1

int pthread_create(unsigned *thread, const void *attr, void *(*fn)(void *), void *arg);
int pthread_join(unsigned thread, void **retval);

static struct console con;
static int opt_stats, opt_stage = 1, opt_seconds, opt_usb;
static int opt_acm_mode = 9;        /* 9 is the ACM's USB-audio request; 11 is AM-AS/AUX */
static int opt_poll_us = 100000;
static int expected_len;

static volatile int running = 1;
static unsigned long frames, drawn, superseded;
static unsigned long long t_start, rx_us, rx_bytes, last_rx_us;
static unsigned long long src_dt_us;
static unsigned src_frames, last_ts;
static int have_ts;

/* the climate window: the input thread raises hmi_until_ms on a climate key and the main
 * loop opens the window between two messages. while it is open the player keeps reading
 * the link and handles every LR_AUDIO message normally; a video message is discarded (and
 * acked stale) instead of being painted, so the PCM stream never stops. */
static volatile unsigned long hmi_until_ms;
static volatile int climate_held;
static volatile int reverse_hold;   /* vehicle.state gear is reverse: keep the window open */
static volatile int hmi_from;       /* HMI_FROM_*: what last extended the window, for the log */
static int hmi_active;
static int hmi_resume_pending;      /* fallback: SIGCONT waits for a fresh buttons service */
static int hmi_video_pending;       /* the layer helper is attaching after a window */
static int hmi_video_ready;         /* its surface is mapped; wait for buttons to stop */

/* nothing writes the bulk-out pipe from the thread that produces a message: the input
 * thread and the frame loop queue, and one writer thread drains input first, then the
 * newest ack, then the ready heartbeat. so a touch never waits behind an in-flight ack,
 * and an ack never stalls the loop that has to keep reading frames on a slow guest. a
 * full queue drops the message (touch) or the credit (ack), never blocks. */
#define OUT_MAX 32

static unsigned char qin[OUT_MAX][8], qlenin[OUT_MAX], qack[OUT_MAX][8];
static unsigned qhead_in, qtail_in, qhead_ack, qtail_ack;
static volatile int qlock, ready_pending, caps_pending;
static unsigned long dropped_in, dropped_ack;

/* the writer sleeps on this instead of polling: an ack queued the moment a frame lands
 * must not wait out a fixed tick before the host can see it (the host's latency sample
 * is send-to-ack, and a touch on a slow link must go out now, not at the next millisecond) */
typedef union {
    long long _align;
    unsigned char _pad[64];
} out_sem_t;

int sem_init(out_sem_t *sem, int pshared, unsigned value);
int sem_post(out_sem_t *sem);
int sem_timedwait(out_sem_t *sem, const struct timespec *abs);
static out_sem_t out_sem;

/* the buttons service is restarted for a climate/reverse window, but hmi_events_window_open()
 * can wait a second or more for ham's respawn. doing that on the main loop would leave the
 * PCM ring dry for the start of every window, so a worker owns it and the loop keeps
 * reading/playing audio. states are small enough to guard with the same test-and-set lock
 * the queues use. */
#define BTN_IDLE     0
#define BTN_OPENING  1
#define BTN_LIVE     2
#define BTN_CLOSING  3
static volatile int btn_lock;
static int btn_state;
static int btn_close_wanted;
static int btn_open_wanted;
static int btn_thread_ready;
static int btn_sync_live;           /* fallback when the worker could not be started */
static unsigned btn_tid;
static out_sem_t btn_sem;

/* ---- audio: PCM playback and microphone capture ----------------------------------------
 *
 * the host multiplexes 20 ms PCM chunks into the video stream. playback is a small ring
 * and one thread that always writes a whole chunk: when the ring has nothing it writes
 * silence, so the codec never underruns and the next real chunk starts exactly on a chunk
 * boundary. the microphone runs the other way on a second thread; the writer thread puts
 * its LI_AUDIO messages behind touch/acks so a call never delays the panel. */
#define AUDIO_RING_BYTES (256 * 1024)
/* four 20 ms chunks (80 ms) cover one full 768 KB picture on the link (about 42 ms) and
 * the PCM queued behind it, which is the worst wait left now that the host's video pipe is
 * small; the old 60 ms start sat exactly on that edge. an underrun raises the target by one
 * chunk up to 120 ms; a long clean run lowers it again, so a noisy link trades latency for
 * smoothness and a clean one gets the latency back. */
#define AUDIO_PREBUFFER_MS_MIN  80
#define AUDIO_PREBUFFER_MS_MAX  120
#define AUDIO_PREBUFFER_MS_STEP 20
#define AUDIO_PREBUFFER_BYTES(ms) \
    ((size_t)(AUDIO_OUT_RATE * AUDIO_OUT_CHANNELS * 2) * (ms) / 1000)
/* the ring is 256 KB (1.45 s) only so a burst never loses data; letting the queue sit
 * there would turn a slow codec clock or a drifting host clock into more than a second
 * of delay before the first drop. keep it near real time: 240 ms covers the 120 ms
 * adaptive target plus the host's six-chunk (120 ms) burst behind a full picture. */
#define AUDIO_QUEUE_MS_MAX     240
#define MIC_QUEUE        32
#define MIC_MSG_MAX      (LI_AUDIO_HDR_LEN + 512)

static void q_lock(void);
static void q_unlock(void);
static unsigned long now_ms(void);

static int opt_audio = 1;
static struct qa_pcm pcm_out, pcm_in;

static struct {
    unsigned char ring[AUDIO_RING_BYTES];
    size_t wr, rd;
    int enabled, running;
    volatile int stop;
    int chunk;
    size_t pre;                 /* target fill in bytes, independent of the device frag */
    unsigned tid;
    unsigned long chunks, drops, underruns, errors, stable;
} aout;

static unsigned char audio_out_buf[65536] __attribute__((aligned(16)));
static unsigned char mic_raw[65536] __attribute__((aligned(16)));
static unsigned char mic_acc[512];
static int mic_acc_len;
static double mic_pos;
static int mic_prev;
static int mic_have_prev;

static struct {
    int running;
    volatile int stop;
    int chunk;
    unsigned tid;
    unsigned long chunks, errors;
} ain;

static volatile int audio_lock;
static out_sem_t audio_sem;

static unsigned char qmic[MIC_QUEUE][MIC_MSG_MAX];
static unsigned short qmic_len[MIC_QUEUE];
static unsigned qhead_mic, qtail_mic;
static unsigned long mic_drops;
static unsigned mic_seq;
static volatile int mic_cfg_pending;

static unsigned char audio_buf[AUDIO_MAX_PAYLOAD];

static int audio_out_reopen(void);
static int mic_reopen(void);

static void a_lock(void)
{
    while (__sync_lock_test_and_set(&audio_lock, 1)) {
    }
}

static void a_unlock(void)
{
    audio_lock = 0;
}

static unsigned rd16le(const unsigned char *p)
{
    return p[0] | (unsigned)p[1] << 8;
}

static unsigned rd32le(const unsigned char *p)
{
    return p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16 | (unsigned)p[3] << 24;
}

static void put32le(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static size_t audio_level(void)
{
    size_t n;

    a_lock();
    n = aout.wr - aout.rd;
    a_unlock();
    return n;
}

static int audio_pop(unsigned char *dst, size_t n)
{
    size_t avail, i;

    a_lock();
    avail = aout.wr - aout.rd;
    if (avail < n) {
        a_unlock();
        return 0;
    }
    for (i = 0; i < n; i++) {
        dst[i] = aout.ring[(aout.rd + i) & (AUDIO_RING_BYTES - 1)];
    }
    aout.rd += n;
    a_unlock();
    return 1;
}

static void audio_push(const unsigned char *src, size_t n)
{
    size_t space, drop, level, cap, i;

    if (!aout.enabled || n == 0 || n > AUDIO_RING_BYTES) {
        return;
    }
    a_lock();
    space = AUDIO_RING_BYTES - (aout.wr - aout.rd);
    if (n > space) {
        drop = n - space;
        aout.rd += drop;
        aout.drops++;
    }
    level = aout.wr - aout.rd;
    cap = AUDIO_PREBUFFER_BYTES(AUDIO_QUEUE_MS_MAX);
    if (level + n > cap) {
        drop = level + n - cap;
        if (drop > level) {
            drop = level;
        }
        aout.rd += drop;
        aout.drops++;
    }
    for (i = 0; i < n; i++) {
        aout.ring[(aout.wr + i) & (AUDIO_RING_BYTES - 1)] = src[i];
    }
    aout.wr += n;
    a_unlock();
    sem_post(&audio_sem);
}

static void *audio_out_thread(void *arg)
{
    unsigned char *buf = audio_out_buf;
    int playing = 0;

    (void)arg;
    while (!aout.stop) {
        if (!playing && audio_level() < aout.pre) {
            struct timespec ts;

            /* the codec has to keep running while the ring refills: a dry driver can
             * come back in a state its own recovery cannot leave (the close/reopen path
             * is the last resort, not the normal one). write silence, then look again. */
            memset(buf, 0, (size_t)aout.chunk);
            if (qa_write(&pcm_out, buf, aout.chunk) < 0) {
                if (aout.stop) {
                    break;
                }
                aout.errors++;
                (void)qa_prepare(&pcm_out);
                if (audio_out_reopen() < 0) {
                    usleep(200000);
                }
            }
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 10000000;
            while (ts.tv_nsec >= 1000000000) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000;
            }
            sem_timedwait(&audio_sem, &ts);
            continue;
        }
        playing = 1;
        if (!audio_pop(buf, (size_t)aout.chunk)) {
            /* a gap: silence goes out now and the target grows by one chunk, so a burst
             * of late audio is collected before the next start instead of clicking
             * through another underrun. the loop above keeps the codec fed meanwhile. */
            memset(buf, 0, (size_t)aout.chunk);
            aout.underruns++;
            if (aout.pre < AUDIO_PREBUFFER_BYTES(AUDIO_PREBUFFER_MS_MAX)) {
                aout.pre += AUDIO_PREBUFFER_BYTES(AUDIO_PREBUFFER_MS_STEP);
                if (aout.pre > AUDIO_PREBUFFER_BYTES(AUDIO_PREBUFFER_MS_MAX)) {
                    aout.pre = AUDIO_PREBUFFER_BYTES(AUDIO_PREBUFFER_MS_MAX);
                }
            }
            aout.stable = 0;
            playing = 0;
        } else if (++aout.stable > 500 &&
                   aout.pre > AUDIO_PREBUFFER_BYTES(AUDIO_PREBUFFER_MS_MIN)) {
            aout.pre -= AUDIO_PREBUFFER_BYTES(AUDIO_PREBUFFER_MS_STEP);
            if (aout.pre < AUDIO_PREBUFFER_BYTES(AUDIO_PREBUFFER_MS_MIN)) {
                aout.pre = AUDIO_PREBUFFER_BYTES(AUDIO_PREBUFFER_MS_MIN);
            }
            aout.stable = 0;
        }
        if (qa_write(&pcm_out, buf, aout.chunk) < 0) {
            if (aout.stop) {
                break;
            }
            aout.errors++;
            if (aout.errors == 1 || (aout.errors % 100) == 0) {
                printf("rawplay: audio output error %lu (last %d), reopening\n",
                       aout.errors, pcm_out.last_error);
                jlog("rawplay: audio output error %lu (last %d), reopening",
                     aout.errors, pcm_out.last_error);
            }
            (void)qa_prepare(&pcm_out);
            if (audio_out_reopen() < 0) {
                usleep(200000);
            }
        } else {
            aout.chunks++;
        }
    }
    return arg;
}

static void audio_out_stop(void)
{
    if (!aout.running) {
        return;
    }
    aout.stop = 1;
    sem_post(&audio_sem);
    pthread_join(aout.tid, 0);
    aout.running = 0;
    aout.enabled = 0;
    qa_close(&pcm_out);
}

static int audio_out_reopen(void)
{
    int rate = pcm_out.rate, channels = pcm_out.channels;
    int card = pcm_out.card, device = pcm_out.device;

    qa_close(&pcm_out);
    if (qa_open_playback_at(&pcm_out, card, device, rate, channels,
                            AUDIO_OUT_FRAG_BYTES) < 0) {
        return -1;
    }
    pcm_out.stop = &aout.stop;
    if (pcm_out.frag > 0 && pcm_out.frag <= (int)sizeof audio_out_buf) {
        aout.chunk = pcm_out.frag;
    }
    return 0;
}

/* on the real car the ACM only unmutes the ICC's stereo/media PCM when its source is
 * USB audio; card 0 device 0 is that path (confirmed with `wave -a0:0`). the mono/voice
 * device is not present on this firmware, so keep it only as a last-resort fallback. */
static int audio_out_open_best(int rate, int channels)
{
    /* the car's ACM unmutes the ICC media/stereo PCM when its source is USB audio;
     * card 0 device 0 is the confirmed audible path on the real car. keep the others as
     * fallbacks for a board whose preferred device is numbered differently. */
    if (qa_open_playback_at(&pcm_out, 0, 0, rate, channels,
                            AUDIO_OUT_FRAG_BYTES) == 0) {
        printf("rawplay: PCM out on media device 0:0\n");
        return 0;
    }
    if (qa_open_playback(&pcm_out, rate, channels, AUDIO_OUT_FRAG_BYTES) == 0) {
        printf("rawplay: PCM out on preferred device %d:%d\n", pcm_out.card,
               pcm_out.device);
        return 0;
    }
    if (qa_open_playback_at(&pcm_out, 0, 1, rate, channels,
                            AUDIO_OUT_FRAG_BYTES) == 0) {
        printf("rawplay: PCM out on device 0:1\n");
        return 0;
    }
    if (qa_open_playback_at(&pcm_out, 1, 0, rate, channels,
                            AUDIO_OUT_FRAG_BYTES) == 0) {
        printf("rawplay: PCM out on voice device 1:0\n");
        return 0;
    }
    return -1;
}

static void audio_out_config(unsigned fmt, unsigned rate, unsigned channels,
                             unsigned chunk_ms)
{
    if (!opt_audio) {
        return;
    }
    if (fmt != AUDIO_OUT_FORMAT || rate != AUDIO_OUT_RATE ||
        channels != AUDIO_OUT_CHANNELS || chunk_ms != AUDIO_OUT_CHUNK_MS) {
        printf("rawplay: audio format %u %u Hz %u ch %u ms is not supported\n", fmt,
               rate, channels, chunk_ms);
        jlog("rawplay: unsupported audio format %u %u Hz %u ch %u ms", fmt, rate,
             channels, chunk_ms);
        return;
    }
    if (aout.running) {
        /* the host repeats this handshake in its one-second heartbeat. keep the stream
         * and its buffered PCM: resetting the ring here dropped up to the whole prebuffer
         * every second, which is audible on a clean link. the wire format is fixed and
         * was just checked, so there is nothing to reconfigure. */
        return;
    }
    if (audio_out_open_best((int)rate, (int)channels) < 0) {
        printf("rawplay: no PCM playback device (audio stays off)\n");
        jlog("rawplay: no PCM playback device (audio stays off)");
        return;
    }
    pcm_out.stop = &aout.stop;
    aout.chunk = pcm_out.frag > 0 && pcm_out.frag <= (int)sizeof audio_out_buf
                     ? pcm_out.frag : AUDIO_OUT_CHUNK_BYTES;
    aout.rd = aout.wr = 0;
    aout.chunks = aout.drops = aout.underruns = aout.errors = aout.stable = 0;
    aout.pre = AUDIO_PREBUFFER_BYTES(AUDIO_PREBUFFER_MS_MIN);
    aout.stop = 0;
    aout.enabled = 1;
    if (pthread_create(&aout.tid, 0, audio_out_thread, 0)) {
        printf("rawplay: can't start the audio thread\n");
        jlog("rawplay: can't start the audio thread");
        aout.enabled = 0;
        qa_close(&pcm_out);
        return;
    }
    aout.running = 1;
    printf("rawplay: PCM out %u Hz %u ch s16le, %d ms wire chunks, %d ms frag %d, "
           "prebuffer %d ms\n", rate, channels, AUDIO_OUT_CHUNK_MS, AUDIO_OUT_FRAG_MS,
           pcm_out.frag, AUDIO_PREBUFFER_MS_MIN);
    jlog("rawplay: PCM out %u Hz %u ch s16le, %d ms wire chunks, %d ms frag %d, "
         "prebuffer %d ms", rate, channels, AUDIO_OUT_CHUNK_MS, AUDIO_OUT_FRAG_MS,
         pcm_out.frag, AUDIO_PREBUFFER_MS_MIN);
}

static void mic_push(const unsigned char *pcm, int len)
{
    unsigned t = qtail_mic, next = (t + 1) % MIC_QUEUE;
    unsigned char *m;

    if (len <= 0 || len > 512) {
        return;
    }
    q_lock();
    if (next == qhead_mic) {
        qhead_mic = (qhead_mic + 1) % MIC_QUEUE;
        mic_drops++;
    }
    m = qmic[t];
    m[0] = 'L';
    m[1] = 'I';
    m[2] = LI_AUDIO;
    m[3] = (unsigned char)len;
    m[4] = (unsigned char)(len >> 8);
    put32le(m + 5, mic_seq++);
    put32le(m + 9, now_ms());
    memcpy(m + LI_AUDIO_HDR_LEN, pcm, (size_t)len);
    qmic_len[t] = (unsigned short)(LI_AUDIO_HDR_LEN + len);
    qtail_mic = next;
    q_unlock();
    sem_post(&out_sem);
}

static int pop_mic(unsigned char *dst)
{
    unsigned h = qhead_mic;
    int n;

    q_lock();
    if (h == qtail_mic) {
        q_unlock();
        return 0;
    }
    n = qmic_len[h];
    memcpy(dst, qmic[h], (size_t)n);
    qhead_mic = (h + 1) % MIC_QUEUE;
    q_unlock();
    return n;
}

static void mic_emit(unsigned char v)
{
    mic_acc[mic_acc_len++] = v;
    if (mic_acc_len >= AUDIO_IN_CHUNK_BYTES) {
        mic_push(mic_acc, AUDIO_IN_CHUNK_BYTES);
        memmove(mic_acc, mic_acc + AUDIO_IN_CHUNK_BYTES,
                (size_t)(mic_acc_len - AUDIO_IN_CHUNK_BYTES));
        mic_acc_len -= AUDIO_IN_CHUNK_BYTES;
        ain.chunks++;
    }
}

/* the codec capture is s16 stereo at its own rate; average to mono, resample to 8 kHz by
 * linear interpolation (the state carries across fragment boundaries) and convert to u8. */
static void mic_convert_native(const unsigned char *raw, int n)
{
    const short *s = (const short *)raw;
    int ch = pcm_in.channels > 1 ? pcm_in.channels : 1;
    int frames = n / (2 * ch);
    double step = (double)pcm_in.rate / AUDIO_IN_RATE;
    int f;

    for (f = 0; f < frames; f++) {
        int l = s[f * ch];
        int r = ch > 1 ? s[f * ch + 1] : l;
        int mono = (l + r) / 2;

        if (!mic_have_prev) {
            mic_prev = mono;
            mic_have_prev = 1;
            mic_pos = 1.0;
            continue;
        }
        while (mic_pos <= 1.0) {
            double v = mic_prev + (mono - mic_prev) * mic_pos;
            int u = (int)(v / 256) + 128;

            if (u < 0) {
                u = 0;
            } else if (u > 255) {
                u = 255;
            }
            mic_emit((unsigned char)u);
            mic_pos += step;
        }
        mic_pos -= 1.0;
        mic_prev = mono;
    }
}

static void *mic_thread(void *arg)
{
    (void)arg;
    mic_acc_len = 0;
    mic_pos = 1.0;
    mic_have_prev = 0;
    while (!ain.stop) {
        int want = pcm_in.frag > 0 && pcm_in.frag <= (int)sizeof mic_raw
                       ? pcm_in.frag : 4096;
        int n = qa_read(&pcm_in, mic_raw, want);

        if (n < 0) {
            if (ain.stop) {
                break;
            }
            ain.errors++;
            if (qa_prepare(&pcm_in) < 0) {
                if (ain.errors == 1 || (ain.errors % 100) == 0) {
                    printf("rawplay: microphone error %lu (last %d), retrying\n",
                           ain.errors, pcm_in.last_error);
                    jlog("rawplay: microphone error %lu (last %d), retrying",
                         ain.errors, pcm_in.last_error);
                }
                if (mic_reopen() < 0) {
                    usleep(200000);
                }
            }
            continue;
        }
        if (pcm_in.format == QA_FMT_U8) {
            for (int i = 0; i < n; i++) {
                mic_emit(mic_raw[i]);
            }
        } else {
            mic_convert_native(mic_raw, n);
        }
    }
    return arg;
}

static void mic_stop(void)
{
    if (!ain.running) {
        return;
    }
    ain.stop = 1;
    pthread_join(ain.tid, 0);
    ain.running = 0;
    qa_close(&pcm_in);
}

static int mic_open_best(void)
{
    /* the voice/mono capture is the path the car's ACM feeds the ICC microphone/voice
     * signal into; the stereo "Mic In" device is the fallback. */
    if (qa_open_capture_at(&pcm_in, 1, 0, 8000, 1, 160) == 0) {
        printf("rawplay: PCM in on voice device 1:0\n");
        return 0;
    }
    if (qa_open_capture_at(&pcm_in, 0, 1, 8000, 1, 160) == 0) {
        printf("rawplay: PCM in on device 0:1\n");
        return 0;
    }
    if (qa_open_capture_native(&pcm_in, 44100, 2, 4096) == 0
        || qa_open_capture_native(&pcm_in, 48000, 2, 4096) == 0
        || qa_open_capture(&pcm_in, AUDIO_IN_RATE, AUDIO_IN_CHANNELS,
                           AUDIO_IN_CHUNK_BYTES) == 0) {
        printf("rawplay: PCM in on preferred device %d:%d\n", pcm_in.card,
               pcm_in.device);
        return 0;
    }
    return -1;
}

static int mic_reopen(void)
{
    qa_close(&pcm_in);
    if (mic_open_best() < 0) {
        return -1;
    }
    pcm_in.stop = &ain.stop;
    mic_acc_len = 0;
    mic_have_prev = 0;
    mic_pos = 1.0;
    return 0;
}

static void mic_start(void)
{
    if (!opt_audio || ain.running) {
        return;
    }
    if (mic_open_best() < 0) {
        printf("rawplay: no PCM capture device (microphone stays off)\n");
        jlog("rawplay: no PCM capture device (microphone stays off)");
        return;
    }
    pcm_in.stop = &ain.stop;
    ain.stop = 0;
    ain.chunks = ain.errors = 0;
    if (pthread_create(&ain.tid, 0, mic_thread, 0)) {
        printf("rawplay: can't start the microphone thread\n");
        jlog("rawplay: can't start the microphone thread");
        qa_close(&pcm_in);
        return;
    }
    ain.running = 1;
    if (pcm_in.format == QA_FMT_U8) {
        printf("rawplay: PCM in %u Hz %u ch u8, %d ms chunks, frag %d\n",
               AUDIO_IN_RATE, AUDIO_IN_CHANNELS, AUDIO_IN_CHUNK_MS, pcm_in.frag);
        jlog("rawplay: PCM in %u Hz %u ch u8, %d ms chunks, frag %d", AUDIO_IN_RATE,
             AUDIO_IN_CHANNELS, AUDIO_IN_CHUNK_MS, pcm_in.frag);
    } else {
        printf("rawplay: PCM in %d Hz %d ch s16 -> %u Hz u8, frag %d\n", pcm_in.rate,
               pcm_in.channels, AUDIO_IN_RATE, pcm_in.frag);
        jlog("rawplay: PCM in %d Hz %d ch s16 -> %u Hz u8, frag %d", pcm_in.rate,
             pcm_in.channels, AUDIO_IN_RATE, pcm_in.frag);
    }
}

static void q_lock(void)
{
    while (__sync_lock_test_and_set(&qlock, 1)) {
    }
}

static void q_unlock(void)
{
    qlock = 0;
}

static unsigned char rxbuf[1024];
static int rxpos, rxlen;
static unsigned char *discard_buf;
static const char *exit_reason;

static long long now_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static unsigned long now_ms(void)
{
    return (unsigned long)(now_us() / 1000);
}

static int is_climate_key(int bit)
{
    for (unsigned i = 0; i < sizeof climate_keys; i++) {
        if (climate_keys[i] == bit) {
            return 1;
        }
    }
    return 0;
}

/* raise or extend the hmi window: the input thread and the vehicle reader both call this,
 * and the main loop notices between messages. moving hmi_until_ms forward is enough: a
 * press while the previous window is closing keeps the window open (and a press after it
 * closed opens it again). */
static void note_hmi_window(int from)
{
    hmi_from = from;
    __sync_synchronize();
    hmi_until_ms = now_ms() + HMI_WINDOW_MS;
}

/* every panel event the input thread sees passes through here. a climate key extends the
 * window; the main loop notices after the frame it is on and brings the hmi back. */
static void note_climate(const struct input_event *ev)
{
    if (!opt_hmi || ev->type != INPUT_BUTTON || !is_climate_key(ev->code)) {
        return;
    }
    if (ev->down) {
        climate_held++;
    } else if (climate_held > 0) {
        climate_held--;
    }
    note_hmi_window(HMI_FROM_CLIMATE);
}

/* ask the v850 for the current vehicle state, the flow message hmictl.c's car guard sends
 * when a watcher starts mid-session: without it a player started while reverse is already
 * engaged would not know until the gear moves again. */
static void vehicle_request_state(void)
{
    static const unsigned char flow[] = { 0x03, 0x02, 0x02 };
    int fd = open("/dev/ipc/4", O_WRONLY);

    if (fd >= 0) {
        write(fd, flow, sizeof flow);
        close(fd);
    }
}

/* the ACM gates the ICC's analogue output by its selected source. the ACM/HMI source table
 * (acm 0x131530) maps the HMI's USB source request (event 10) to v850 event 5 with data 9;
 * data 11 is the AM-AS request, which the real car shows as AUX/AM-AS. force the USB
 * request by default; --acm-mode -1 leaves the source alone for a car that prefers the
 * ACM's own USB selection when the PCM device opens. */
static int acm_select_source(int mode)
{
    static const unsigned char flow[] = { 0x0f, 0x03, 0x02 };
    unsigned char request[] = { 0x10, 0x03, 0x05, 0, 0, 0, (unsigned char)mode };
    int fd = open("/dev/ipc/2", O_WRONLY);

    if (fd < 0) {
        return -1;
    }
    /* ask the v850 for its current acm state, then send the source request. the acm
     * process does the same flow request when it connects, but rawplay may start long
     * after that connection and the source request still has to go through. */
    write(fd, flow, sizeof flow);
    write(fd, request, sizeof request);
    close(fd);
    return 0;
}

/* the car's side of the reverse window: only vehicle.state matters. only rx frames
 * matter (direction byte 1 set is the guest's own tx). the monitor can go quiet after a
 * stopped hmi's resume, so a read error or 5 s without a frame reopens it, the same
 * self-healing the SDK's input.c panel reader uses. a first frame that already says
 * reverse does open the window. */
static void *vehicle_thread(void *arg)
{
    static unsigned char buf[1002];
    int fd = -1;
    unsigned silent = 0;
    ssize_t n;

    (void)arg;
    for (;;) {
        if (fd < 0) {
            fd = open(IPC_MONITOR, O_RDONLY);
            if (fd < 0) {
                usleep(250000);
                continue;
            }
            silent = 0;
            vehicle_request_state();
        }
        n = read(fd, buf, sizeof buf);
        if (n < 0) {
            close(fd);
            fd = -1;
            usleep(100000);
            continue;
        }
        if (n < 5) {
            if (++silent >= VEHICLE_SILENT) {
                close(fd);
                fd = -1;
            } else {
                usleep(10000);
            }
            continue;
        }
        silent = 0;
        if (buf[1] || buf[0] != VEHICLE_CH) {
            continue;
        }
        if (buf[2] == VSTATE_MSG && buf[3] == VSTATE_VER && (int)n - 2 >= VSTATE_LEN) {
            if (buf[4 + VSTATE_GEAR] == GEAR_REVERSE) {
                if (!reverse_hold) {
                    reverse_hold = 1;
                    note_hmi_window(HMI_FROM_REVERSE);
                }
            } else if (reverse_hold) {
                reverse_hold = 0;
            }
        }
    }
    return arg;
}

static void push_input(const unsigned char *m, int n)
{
    unsigned t = qtail_in, next = (t + 1) % OUT_MAX;

    q_lock();
    if (next == qhead_in) {
        /* a full queue drops the oldest sample, not the newest: during a drag the last
         * position matters and the release must never be the event that is lost */
        qhead_in = (qhead_in + 1) % OUT_MAX;
        dropped_in++;
    }
    memcpy(qin[t], m, n);
    qlenin[t] = n;
    qtail_in = next;
    q_unlock();
    sem_post(&out_sem);
}

static int pop_input(unsigned char *m)
{
    unsigned h = qhead_in;
    int n;

    q_lock();
    if (h == qtail_in) {
        q_unlock();
        return 0;
    }
    n = qlenin[h];
    memcpy(m, qin[h], n);
    qhead_in = (h + 1) % OUT_MAX;
    q_unlock();
    return n;
}

static void push_ack(unsigned seq, int status)
{
    unsigned t = qtail_ack, next = (t + 1) % OUT_MAX;
    unsigned char *m;

    q_lock();
    if (next == qhead_ack) {
        /* the host's window only needs a newer position, so an old credit is worthless */
        qhead_ack = (qhead_ack + 1) % OUT_MAX;
        dropped_ack++;
    }
    m = qack[t];
    m[0] = 'L';
    m[1] = 'I';
    m[2] = LI_RAW;
    m[3] = seq;
    m[4] = seq >> 8;
    m[5] = seq >> 16;
    m[6] = seq >> 24;
    m[7] = (unsigned char)status;
    qtail_ack = next;
    q_unlock();
    sem_post(&out_sem);
}

static int pop_ack(unsigned char *m)
{
    unsigned h = qhead_ack;

    q_lock();
    if (h == qtail_ack) {
        q_unlock();
        return 0;
    }
    memcpy(m, qack[h], 8);
    qhead_ack = (h + 1) % OUT_MAX;
    q_unlock();
    return 8;
}

static void send_ready(void)
{
    ready_pending = 1;
    caps_pending = 1;
    mic_cfg_pending = 1;
    sem_post(&out_sem);
}

static void send_ack(unsigned seq, int status)
{
    push_ack(seq, status);
}

static void send_input(const struct input_event *ev)
{
    unsigned char m[8];
    int n = 0;

    if (ev->type == INPUT_TOUCH) {
        m[0] = 'L';
        m[1] = 'I';
        m[2] = LI_TOUCH;
        m[3] = ev->x;
        m[4] = ev->x >> 8;
        m[5] = ev->y;
        m[6] = ev->y >> 8;
        m[7] = ev->down;
        n = 8;
    } else if (ev->type == INPUT_KNOB && ev->code) {
        m[0] = 'L';
        m[1] = 'I';
        m[2] = LI_KNOB;
        m[3] = ev->code;
        n = 4;
    } else if (ev->type == INPUT_BUTTON) {
        m[0] = 'L';
        m[1] = 'I';
        m[2] = LI_BUTTON;
        m[3] = ev->code;
        m[4] = ev->down;
        n = 5;
    }
    if (n) {
        push_input(m, n);
    }
}

static void *writer_thread(void *arg)
{
    unsigned char m[MIC_MSG_MAX];

    while (running) {
        int n = pop_input(m);

        if (!n) {
            n = pop_ack(m);
        }
        if (!n && mic_cfg_pending) {
            mic_cfg_pending = 0;
            m[0] = 'L';
            m[1] = 'I';
            m[2] = LI_AUDIO_CFG;
            m[3] = 1;                       /* version */
            m[4] = AUDIO_IN_FORMAT;
            m[5] = AUDIO_IN_RATE & 0xff;
            m[6] = AUDIO_IN_RATE >> 8;
            m[7] = AUDIO_IN_CHANNELS;
            m[8] = AUDIO_IN_CHUNK_MS;
            m[9] = aout.running ? (unsigned char)(1 + ((pcm_out.card & 0xf) << 4)
                                                   + (pcm_out.device & 0xf)) : 0xff;
            m[10] = ain.running ? (unsigned char)(1 + ((pcm_in.card & 0xf) << 4)
                                                   + (pcm_in.device & 0xf)) : 0xff;
            m[11] = (unsigned char)(aout.errors > 0xff ? 0xff : aout.errors);
            n = LI_AUDIO_CFG_LEN;
        }
        if (!n && ready_pending) {
            ready_pending = 0;
            m[0] = 'L';
            m[1] = 'I';
            m[2] = LI_READY;
            n = 3;
        }
        if (!n && caps_pending) {
            caps_pending = 0;
            m[0] = 'L';
            m[1] = 'I';
            m[2] = LI_CAPS;
            m[3] = LI_CAPS_VER;
            m[4] = LR_CAP_BANDS;
            m[5] = m[6] = m[7] = 0;
            n = LI_CAPS_LEN;
        }
        if (!n) {
            n = pop_mic(m);
        }
        if (n) {
            raw_usb_write(m, n);
            continue;
        }
        {
            struct timespec ts;

            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 100000000;
            while (ts.tv_nsec >= 1000000000) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000;
            }
            sem_timedwait(&out_sem, &ts);
        }
    }
    return arg;
}

/* the panel owns the video, so the input thread only forwards.
 *
 * the driver reports a sample every millisecond while a finger is down, and a fresh
 * reader gets the samples from before it opened as a burst with the next touch. only the
 * newest sample is useful to the host, so one pass forwards at most one touch: press and
 * move samples coalesce to the newest position, and a release is forwarded only for a
 * press the host was actually told about. that is what keeps a replayed gesture, the tap
 * that launched the player included, from reaching the host as a flood of moves and
 * phantom clicks. exits are the car's: the power button (or audio off with the ignition
 * off) closes the whole homebrew session through hmictl.c's guard. */
static void *input_thread(void *arg)
{
    int host_down = 0;

    while (running) {
        struct input_event ev, pending;
        int have_pending = 0;

        while (input_poll(&ev)) {
            note_climate(&ev);
            if (ev.type != INPUT_TOUCH) {
                if (have_pending) {
                    send_input(&pending);
                    have_pending = 0;
                    host_down = 1;
                }
                send_input(&ev);
                continue;
            }
            if (ev.down) {
                pending = ev;
                have_pending = 1;
                continue;
            }
            if (!host_down) {
                /* a replayed release or the tail of a replayed gesture: the host never
                 * saw the press, so it must not see the release either */
                have_pending = 0;
                continue;
            }
            if (have_pending) {
                send_input(&pending);
                have_pending = 0;
            }
            send_input(&ev);
            host_down = 0;
        }
        if (have_pending) {
            send_input(&pending);
            host_down = 1;
        }
        /* event driven: the touch/panel readers post the queue, so this wakes on the next
         * sample instead of after a poll interval. the timeout is only a lost-wakeup net. */
        input_wait_event(opt_poll_us);
    }
    return arg;
}

/* ---- the byte stream -----------------------------------------------------------
 *
 * every bulk-in request is a whole number of the endpoint's max packets. the device
 * sends maxpacket-sized packets and does not know how much the host asked for, so a
 * request that is not a multiple of the maxpacket makes the host drop the tail of the
 * last packet; the ehci side then errors the transfer (measured on a real car as
 * USBD_STATUS_CMP_ERR/IO on every read, which is what "no video from the host" was).
 * a message tail shorter than a packet is taken through rxbuf, which keeps whatever
 * came with it for the next read instead of losing it. */

static int sget(unsigned char *b)
{
    if (rxpos >= rxlen) {
        /* the first bytes get the longer startup wait: rawplay is usually started just
         * before the host sender. once anything has arrived a 5 s silence is a dead
         * link, and the transport fails the read instead of blocking here forever. */
        int r = raw_usb_read(rxbuf, raw_usb_mps(),
                             rx_bytes ? LINK_DEAD_US / 1000 : STARTUP_DEAD_US / 1000);

        if (r < 0) {
            return -1;
        }
        if (r == 0) {
            return 0;
        }
        rxpos = 0;
        rxlen = r;
    }
    *b = rxbuf[rxpos++];
    return 1;
}

/* read n bytes into dst. the leftovers already buffered are used first; what remains is
 * read in maxpacket multiples straight into dst, except a tail smaller than a packet,
 * which goes through rxbuf and leaves the overshoot buffered. only the panel payload
 * passes allow_direct: protocol bytes must never be a direct-urb destination. */
static int read_exact_mode(unsigned char *dst, int n, int allow_direct)
{
    int done = 0, quiet = 0;

    if (rxlen - rxpos > 0) {
        int take = rxlen - rxpos > n ? n : rxlen - rxpos;

        memcpy(dst, rxbuf + rxpos, take);
        rxpos += take;
        done += take;
    }
    while (done < n) {
        int mps = raw_usb_mps();
        int rem = n - done;
        int r;

        if (rem >= mps) {
            int chunk = rem - rem % mps;

            r = allow_direct ? raw_usb_read_direct(dst + done, chunk, LINK_DEAD_US / 1000)
                             : raw_usb_read(dst + done, chunk, LINK_DEAD_US / 1000);
            if (r < 0) {
                return -1;      /* dead, quiet or stopped; without this the loop spins */
            }
            if (r > 0) {
                quiet = 0;
                done += r;
            }
        } else {
            int take;

            r = raw_usb_read(rxbuf, mps, LINK_DEAD_US / 1000);
            if (r < 0) {
                return -1;
            }
            if (r > 0) {
                quiet = 0;
                take = r > rem ? rem : r;
                memcpy(dst + done, rxbuf, take);
                rxpos = take;
                rxlen = r;
                done += take;
                continue;
            }
        }
        if (r == 0 && ++quiet >= 10) {
            return -1;              /* 5 s with nothing is a dead link, not a slow one */
        }
    }
    return 0;
}

static int read_exact(unsigned char *dst, int n)
{
    return read_exact_mode(dst, n, 0);
}

/* the frame payload is the one transfer that may dma straight into the panel mapping */
static int read_exact_panel(unsigned char *dst, int n)
{
    return read_exact_mode(dst, n, 1);
}

static int discard(int n)
{
    int quiet = 0;
    int maxw = raw_usb_scratch_size();

    while (n > 0) {
        int mps = raw_usb_mps();
        int r;

        if (rxlen - rxpos > 0) {
            int take = rxlen - rxpos > n ? n : rxlen - rxpos;

            rxpos += take;
            n -= take;
            continue;
        }
        if (n >= mps) {
            int chunk = n - n % mps;

            if (chunk > maxw) {
                chunk = maxw;       /* the scratch buffer is one urb's worth */
            }
            r = raw_usb_read(discard_buf, chunk, LINK_DEAD_US / 1000);
        } else {
            /* a sub-packet tail: through rxbuf so the rest of the packet is not lost */
            r = raw_usb_read(rxbuf, mps, LINK_DEAD_US / 1000);
            if (r > 0) {
                int take = r > n ? n : r;

                rxpos = take;
                rxlen = r;
                n -= take;
                continue;
            }
        }
        if (r < 0) {
            return -1;
        }
        if (++quiet >= 10) {
            return -1;              /* nothing for ten reads: the main loop's watchdog */
        }
        if (r == 0) {
            continue;
        }
        quiet = 0;
        n -= r;
    }
    return 0;
}

/* the host pads every LR message to one max packet (512 bytes): the 16-byte header is
 * followed by pad bytes, so the payload (or the next message) starts on a packet boundary.
 * the receiver skips the pad here. it is normally already buffered (the header read is one
 * maxpacket), but if it is not, another packet is read. without the padding a frame ended
 * in a sub-packet remainder whose 512-byte read could not complete until the *next* frame
 * started arriving: that pinned every ack to the next frame (a frame period of latency)
 * and deadlocked the link when the host's window held only one frame. */
static int skip_pad(int n)
{
    while (n > 0) {
        int have = rxlen - rxpos;

        if (have > 0) {
            int take = have > n ? n : have;

            rxpos += take;
            n -= take;
            continue;
        }
        {
            int r = raw_usb_read(rxbuf, raw_usb_mps(), LINK_DEAD_US / 1000);

            if (r <= 0) {
                return -1;
            }
            rxpos = 0;
            rxlen = r;
        }
    }
    return 0;
}

/* a bulk-in transfer error leaves the stream mid-frame with no in-band resync: drop the
 * transport's buffered bytes, let it drain and reset its pipe, and let the main loop scan
 * for the next LR header. the frame that was in flight is acked stale by the caller so
 * the host's window moves on. */
static void resync_stream(void)
{
    raw_usb_resync();
    rxpos = rxlen = 0;
}

/* returns 1 on a header, 0 when no bytes are available yet, -1 on a dead link */
static int read_header(unsigned *type, unsigned *len, unsigned *seq, unsigned *ts)
{
    unsigned char h[LR_HEADER];
    unsigned char c, prev = 0;
    int i;

    for (;;) {
        int r = sget(&c);

        if (r < 0) {
            return -1;
        }
        if (r == 0) {
            return 0;
        }
        if (c == LR_MAGIC1 && prev == LR_MAGIC0) {
            break;
        }
        prev = c;
    }
    h[0] = LR_MAGIC0;
    h[1] = LR_MAGIC1;
    for (i = 2; i < LR_HEADER; ) {
        int r = sget(&h[i]);

        if (r < 0) {
            return -1;
        }
        if (r == 0) {
            return 0;               /* let the main loop run its watchdog */
        }
        i++;
    }
    *type = h[2];
    *len = h[4] | h[5] << 8 | h[6] << 16 | (unsigned)h[7] << 24;
    *seq = h[8] | h[9] << 8 | h[10] << 16 | (unsigned)h[11] << 24;
    *ts = h[12] | h[13] << 8 | h[14] << 16 | (unsigned)h[15] << 24;
    /* a raw picture has no mpeg start code to validate against, so the header is checked
     * against the protocol instead: reserved byte zero, a known type and a length that
     * matches the type. a false 'LR' inside picture data fails on the length. */
    if (h[3] != 0 ||
        (*type == LR_MODE && *len != MODE_PAYLOAD) ||
        (*type == LR_FRAME && (!expected_len || *len != (unsigned)expected_len)) ||
        (*type == LR_BANDS && (!expected_len || *len < LR_BAND_MAP ||
                              *len > (unsigned)expected_len + LR_BAND_MAP)) ||
        (*type == LR_AUDIO_CFG && *len != LR_AUDIO_CFG_LEN) ||
        (*type == LR_AUDIO && *len != AUDIO_OUT_CHUNK_BYTES) ||
        (*type != LR_MODE && *type != LR_FRAME && *type != LR_BANDS &&
         *type != LR_AUDIO_CFG && *type != LR_AUDIO)) {
        return 2;                   /* not a header: keep scanning */
    }
    return 1;
}

static int read_mode(void)
{
    unsigned char p[MODE_PAYLOAD];
    unsigned w, h, stride, fmt;

    if (read_exact(p, MODE_PAYLOAD) < 0) {
        return -1;
    }
    if (skip_pad(LR_MSG - LR_HEADER - MODE_PAYLOAD) < 0) {
        return -1;
    }
    w = p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24;
    h = p[4] | p[5] << 8 | p[6] << 16 | (unsigned)p[7] << 24;
    stride = p[8] | p[9] << 8 | p[10] << 16 | (unsigned)p[11] << 24;
    fmt = p[12] | p[13] << 8 | p[14] << 16 | (unsigned)p[15] << 24;
    /* bit 8 of the format word is the transport hint: the host promises request-sized
     * bulk transfers (functionfs does; qemu's usb-livi chunked=on models it), which is
     * what lets direct mode place dma at fixed offsets. without it the staged ring is
     * used, because a short completion on the direct ring would misplace the frame. */
    if ((fmt & 0xff) != FMT_RGB565 || (int)w != fb.width || (int)h != fb.height ||
        (int)stride != (int)fb.stride) {
        printf("rawplay: host mode %ux%u stride %u fmt %u does not match panel %dx%d "
               "stride %u\n", w, h, stride, fmt, fb.width, fb.height, fb.stride);
        return -1;
    }
    expected_len = (int)(stride * h);
    raw_usb_set_direct(!opt_stage && (fmt & 0x100));
    printf("rawplay: host mode %ux%u rgb565, %d bytes/frame, path %s\n", w, h, expected_len,
           raw_usb_direct() ? "direct" : "staged");
    jlog("rawplay: host mode %ux%u rgb565, %d bytes/frame, path %s", w, h, expected_len,
         raw_usb_direct() ? "direct" : "staged");
    return 0;
}

/* the audio config starts both directions: playback on the unit and the microphone back
 * to the host. it is repeated by the host whenever the reader is reset. */
static int read_audio_cfg(void)
{
    unsigned char p[LR_AUDIO_CFG_LEN];

    if (read_exact(p, sizeof p) < 0) {
        return -1;
    }
    if (skip_pad(LR_MSG - LR_HEADER - sizeof p) < 0) {
        return -1;
    }
    audio_out_config(rd32le(p), rd32le(p + 4), rd32le(p + 8), rd32le(p + 12));
    mic_start();
    return 0;
}

/* a PCM chunk is the 512-byte header then the payload, zero padded to the next whole
 * 512-byte boundary so the following header starts on a packet boundary. */
static int read_audio(unsigned len)
{
    int pad;

    if (skip_pad(LR_MSG - LR_HEADER) < 0) {
        return -1;
    }
    if (read_exact(audio_buf, (int)len) < 0) {
        return -1;
    }
    pad = (int)((LR_MSG - (len % LR_MSG)) % LR_MSG);
    if (pad && skip_pad(pad) < 0) {
        return -1;
    }
    audio_push(audio_buf, len);
    return 0;
}

/* changed-row update: one map packet and then each run's raw rgb565 straight into the
 * panel mapping. the host aligns every run to whole packets, so the normal case is the
 * same zero-copy dma as a full frame, just at a row offset. */
static int read_bands(unsigned len)
{
    unsigned char map[LR_BAND_MAP];
    unsigned count, total = 0;
    int pad;

    if (len < LR_BAND_MAP || len > (unsigned)expected_len + LR_BAND_MAP) {
        return -1;
    }
    if (skip_pad(LR_MSG - LR_HEADER) < 0) {
        return -1;
    }
    if (read_exact(map, LR_BAND_MAP) < 0) {
        return -1;
    }
    count = rd16le(map);
    if (count > LR_BAND_MAX) {
        return -1;
    }
    for (unsigned i = 0; i < count; i++) {
        unsigned y = rd16le(map + 4 + i * 4);
        unsigned rows = rd16le(map + 6 + i * 4);

        if (rows == 0 || y + rows > fb.height) {
            return -1;
        }
        total += rows * (unsigned)fb.stride;
    }
    if (total != len - LR_BAND_MAP) {
        return -1;
    }
    for (unsigned i = 0; i < count; i++) {
        unsigned y = rd16le(map + 4 + i * 4);
        unsigned rows = rd16le(map + 6 + i * 4);

        if (read_exact_panel((unsigned char *)fb.pixels + (size_t)y * fb.stride,
                             (int)(rows * (unsigned)fb.stride)) < 0) {
            return -1;
        }
    }
    pad = (int)((LR_MSG - (len % LR_MSG)) % LR_MSG);
    if (pad && skip_pad(pad) < 0) {
        return -1;
    }
    return 0;
}

/* ---- panel --------------------------------------------------------------------- */

static void panel_clear(pixel c)
{
    fb_fill(&fb, 0, 0, fb.width, fb.height, c);
}

static void ui_idle(void)
{
    con_init(&con, &fb);
    con_clear(&con);
    con_printf(&con, "rawplay: waiting for the host\n");
}

/* ---- hmi windows ----------------------------------------------------------------------
 *
 * a climate key or reverse gear drops the video layer so the hmi shows, but the player
 * keeps reading the link: LR_AUDIO messages are played exactly as during the video and a
 * video message is discarded (acked stale) instead of being painted. that is what keeps
 * the PCM stream alive while the climate bar is up. the buttons restart and the return of
 * the layer helper run off the main loop as well, so neither blocks the audio path. */

static void b_lock(void)
{
    while (__sync_lock_test_and_set(&btn_lock, 1)) {
    }
}

static void b_unlock(void)
{
    btn_lock = 0;
}

static void *hmi_buttons_thread(void *arg)
{
    (void)arg;
    for (;;) {
        struct timespec ts;
        int state;

        for (;;) {
            b_lock();
            state = btn_state;
            b_unlock();
            if (state == BTN_OPENING || state == BTN_CLOSING) {
                break;
            }
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 100000000;
            while (ts.tv_nsec >= 1000000000) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000;
            }
            sem_timedwait(&btn_sem, &ts);
        }
        if (state == BTN_OPENING) {
            int live = hmi_events_window_open();

            if (live) {
                printf("rawplay: buttons restarted for the hmi\n");
                jlog("rawplay: buttons restarted for the hmi");
            }
            b_lock();
            if (btn_close_wanted) {
                btn_close_wanted = 0;
                btn_state = live ? BTN_CLOSING : BTN_IDLE;
            } else {
                btn_state = live ? BTN_LIVE : BTN_IDLE;
            }
            b_unlock();
            continue;
        }
        hmi_events_window_close();
        printf("rawplay: buttons stopped with the hmi again\n");
        jlog("rawplay: buttons stopped with the hmi again");
        b_lock();
        if (btn_open_wanted) {
            btn_open_wanted = 0;
            btn_state = BTN_OPENING;
        } else {
            btn_state = BTN_IDLE;
        }
        b_unlock();
    }
    return arg;
}

static void hmi_buttons_request_open(void)
{
    int post = 0;

    if (!btn_thread_ready) {
        if (hmi_events_window_open()) {
            btn_sync_live = 1;
            printf("rawplay: buttons restarted for the hmi\n");
            jlog("rawplay: buttons restarted for the hmi");
        }
        return;
    }
    b_lock();
    if (btn_state == BTN_IDLE) {
        btn_state = BTN_OPENING;
        post = 1;
    } else if (btn_state == BTN_CLOSING) {
        /* the previous window's stop is still running: let it finish, then restart */
        btn_open_wanted = 1;
    } else if (btn_state == BTN_OPENING) {
        /* a newer window opened before the restart finished: it wants the buttons */
        btn_close_wanted = 0;
    }
    b_unlock();
    if (post) {
        sem_post(&btn_sem);
    }
}

static void hmi_buttons_request_close(void)
{
    int post = 0;

    if (!btn_thread_ready) {
        if (btn_sync_live) {
            hmi_events_window_close();
            btn_sync_live = 0;
            printf("rawplay: buttons stopped with the hmi again\n");
            jlog("rawplay: buttons stopped with the hmi again");
        }
        return;
    }
    b_lock();
    if (btn_state == BTN_LIVE) {
        btn_state = BTN_CLOSING;
        post = 1;
    } else if (btn_state == BTN_OPENING) {
        btn_close_wanted = 1;
    } else if (btn_state == BTN_CLOSING) {
        /* a press reopened and closed again while the stop was running: no restart */
        btn_open_wanted = 0;
    }
    b_unlock();
    if (post) {
        sem_post(&btn_sem);
    }
}

static int hmi_buttons_busy(void)
{
    int busy;

    b_lock();
    busy = btn_state == BTN_OPENING || btn_state == BTN_CLOSING;
    b_unlock();
    return busy;
}

static int video_suspended(void)
{
    return hmi_active || hmi_video_pending;
}

static void hmi_window_log_close(void)
{
    if (hmi_from == HMI_FROM_REVERSE) {
        printf("rawplay: reverse window closed, video back\n");
        jlog("rawplay: reverse window closed, video back");
    } else {
        printf("rawplay: climate window closed, video back\n");
        jlog("rawplay: climate window closed, video back");
    }
}

/* drop the video layer and let the hmi show. a new key while the previous window's layer
 * was coming back throws that half-attached layer away and keeps the hmi up. */
static void hmi_window_open(void)
{
    int from = hmi_from;

    if (hmi_video_pending) {
        panel_layer_resume_abort();
        hmi_video_pending = 0;
        hmi_video_ready = 0;
    }
    if (hmi_active) {
        return;
    }
    if (layer_video) {
        panel_layer_hide();
    }
    hmi_buttons_request_open();
    if (!layer_video) {
        /* the hmi was stopped behind the video. wait for the fresh buttons connection
         * before resuming it, or the events queued for the frozen one replay at once.
         * the main loop sends the SIGCONT when the worker reports it is done. */
        hmi_resume_pending = 1;
    }
    hmi_active = 1;
    if (from == HMI_FROM_REVERSE) {
        printf("rawplay: reverse gear, hmi back\n");
        jlog("rawplay: reverse gear, hmi back");
    } else {
        printf("rawplay: climate key, hmi back for %d ms\n", HMI_WINDOW_MS);
        jlog("rawplay: climate key, hmi back for %d ms", HMI_WINDOW_MS);
    }
}

/* a climate window closes HMI_WINDOW_MS after the key; reverse holds it open for as long
 * as the gear stays engaged, however long that is. */
static int hmi_window_done(void)
{
    return !reverse_hold && (long)(now_ms() - hmi_until_ms) >= 0 && !climate_held;
}

static void hmi_window_close(void)
{
    hmi_buttons_request_close();
    hmi_active = 0;
    hmi_resume_pending = 0;
    if (!layer_video) {
        hmi_signal(HMI_SIGSTOP);
        hmi_window_log_close();
        return;
    }
    if (panel_layer_resume_start() != 0) {
        printf("rawplay: no video layer after the hmi window, stopping the hmi\n");
        jlog("rawplay: no video layer after the hmi window, stopping the hmi");
        layer_video = 0;
        if (fb_direct) {
            fb.pixels = fb_direct;
        }
        if (opt_hmi) {
            hmi_signal(HMI_SIGSTOP);
        }
        hmi_window_log_close();
        return;
    }
    hmi_video_pending = 1;
    hmi_video_ready = 0;
}

static void usage(void)
{
    printf("usage: rawplay --usb [seconds] [--stats] [--direct] [--stage] [--hmi] [--urb BYTES]\n"
           "  --usb      the composite livi gadget's vendor interface (the only transport)\n"
           "  --direct   zero-copy: bulk-in urbs point straight at the panel mapping. the\n"
           "             default on the emulator, but on a real unit the first frame lands\n"
           "             and then the bulk-in pipe stops, so staged is the default there\n"
           "  --stage    read through the usbd_alloc staging ring and copy (the default)\n"
           "  --hmi      keep the hmi running and own the overlay layer above it. a climate\n"
           "             panel key shows the hmi for " HMI_WINDOW_STR(HMI_WINDOW_MS) " ms after the last key\n"
           "             so the climate bar shows, then the video comes back. the player keeps\n"
           "             reading and playing PCM while the window is open; video frames are\n"
           "             discarded and acked stale so audio never stops. reverse gear holds the\n"
           "             window open for as long as the gear is engaged. day/night needs no\n"
           "             window: the running hmi themes itself offscreen. if the overlay cannot\n"
           "             be attached, fall back to stopping the hmi behind the video\n"
           "  --urb      bulk-in urb size in bytes (default 262144, min 512, max 1048576)\n"
           "  --poll-us  input wake timeout in microseconds, the lost-wakeup net for the\n"
           "             event driven input thread (default 100000, min 250)\n"
           "  --acm-mode N  force an ACM source at startup (default 9, USB audio; -1\n"
           "             leaves the source alone; 11 is AM-AS/AUX on the real car)\n"
           "  --no-audio do not open PCM playback/capture even when the host offers it\n");
}

int main(int argc, char **argv)
{
    long long last_ready = 0, last_stats = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--layer-helper") && i + 4 < argc) {
            return layer_helper(argv[i + 1], (unsigned)strtoul(argv[i + 2], 0, 0),
                                (unsigned)strtoul(argv[i + 3], 0, 0),
                                (pid_t)strtoul(argv[i + 4], 0, 0));
        }
    }
    self_path = argv[0];
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--usb")) {
            opt_usb = 1;
        } else if (!strcmp(argv[i], "--stage")) {
            opt_stage = 1;
        } else if (!strcmp(argv[i], "--direct")) {
            opt_stage = 0;
        } else if (!strcmp(argv[i], "--hmi")) {
            opt_hmi = 1;
        } else if (!strcmp(argv[i], "--no-audio")) {
            opt_audio = 0;
        } else if (!strcmp(argv[i], "--acm-mode") && i + 1 < argc) {
            opt_acm_mode = (int)strtol(argv[++i], 0, 0);
            if (opt_acm_mode < -1 || opt_acm_mode > 255) {
                opt_acm_mode = -1;
            }
        } else if (!strcmp(argv[i], "--urb") && i + 1 < argc) {
            raw_usb_set_chunk((int)strtol(argv[++i], 0, 0));
        } else if (!strcmp(argv[i], "--poll-us") && i + 1 < argc) {
            opt_poll_us = (int)strtol(argv[++i], 0, 0);
            if (opt_poll_us < 250) {
                opt_poll_us = 250;
            }
            if (opt_poll_us > 1000000) {
                opt_poll_us = 1000000;
            }
        } else if (!strcmp(argv[i], "--stats")) {
            opt_stats = 1;
        } else if (argv[i][0] == '-' && argv[i][1] == '-') {
            printf("rawplay: unknown option %s\n", argv[i]);
            usage();
            return 1;
        } else {
            opt_seconds = (int)strtol(argv[i], 0, 0);
        }
    }
    if (!opt_usb) {
        printf("rawplay: the raw path runs over usb only (--usb)\n");
        usage();
        return 1;
    }
    printf("rawplay: video path %s\n", opt_stage ? "staged" : "direct");
    jlog("rawplay: video path %s", opt_stage ? "staged" : "direct");
    if (opt_acm_mode < 0) {
        printf("rawplay: ACM source left alone\n");
        jlog("rawplay: ACM source left alone");
    }
    if (fb_open(&fb) != 0) {
        printf("rawplay: can't map the framebuffer\n");
        jlog("rawplay: can't map the framebuffer");
        return 1;
    }
    /* the hmi keeps running: the layer helper owns the overlay above it, and only drops
     * that overlay for a window. only if the helper cannot take the layer does the old
     * stop/resume session behaviour apply. */
    if (panel_layer_open() == 0) {
        if (opt_hmi) {
            hmi_signal(HMI_SIGCONT);
        }
    } else if (opt_hmi) {
        hmi_signal(HMI_SIGSTOP);
    }
    panel_clear(fb_rgb(10, 14, 24));
    ui_idle();
    printf("rawplay: framebuffer %dx%d stride %u\n", fb.width, fb.height, fb.stride);
    jlog("rawplay: start, framebuffer %dx%d stride %u, urb %d", fb.width, fb.height,
         fb.stride, raw_usb_chunk());
    input_start();
    input_flush();      /* not this run's input; the rest of the driver's history arrives
                         * with the first touch, where the input thread forwards only the
                         * newest state and turns the replayed gestures away */

    if (raw_usb_start() != 0) {
        printf("rawplay: no livi usb gadget found\n");
        jlog("rawplay: no livi usb gadget found");
        panel_finish();
        return 1;
    }
    /* a 64-byte bulk endpoint means the unit's io-usb came up with force_fs. raw 768 KB
     * frames cannot run at 12 mbit, so do not start the stream and paint a slideshow: hand
     * a distinct exit code back to rawplay.sh, which restarts the usb stack high speed and
     * starts the player once more. this also covers a stock/media restart that put force_fs
     * back after the hook had already fixed the boot. */
    if (raw_usb_mps() < 512) {
        printf("rawplay: full-speed usb link, restarting it high speed\n");
        jlog("rawplay: full-speed usb link, restarting it high speed");
        panel_finish();
        return 75;
    }
    if (opt_acm_mode >= 0) {
        if (acm_select_source(opt_acm_mode) == 0) {
            printf("rawplay: ACM source set to %d\n", opt_acm_mode);
            jlog("rawplay: ACM source set to %d", opt_acm_mode);
        } else {
            printf("rawplay: can't open the ACM channel, source left alone\n");
            jlog("rawplay: can't open the ACM channel, source left alone");
        }
    }
    discard_buf = raw_usb_scratch();
    sem_init(&out_sem, 0, 0);
    sem_init(&audio_sem, 0, 0);
    if (opt_hmi) {
        sem_init(&btn_sem, 0, 0);
    }
    send_ready();
    t_start = now_us();
    last_rx_us = t_start;

    {
        unsigned thread_id;

        if (pthread_create(&thread_id, 0, input_thread, 0)) {
            printf("rawplay: can't start the input thread\n");
            panel_finish();
            return 1;
        }
        if (pthread_create(&thread_id, 0, writer_thread, 0)) {
            printf("rawplay: can't start the writer thread\n");
            panel_finish();
            return 1;
        }
        if (opt_hmi && pthread_create(&thread_id, 0, vehicle_thread, 0)) {
            printf("rawplay: can't start the vehicle thread\n");
            panel_finish();
            return 1;
        }
        if (opt_hmi) {
            if (pthread_create(&btn_tid, 0, hmi_buttons_thread, 0) == 0) {
                btn_thread_ready = 1;
            } else {
                printf("rawplay: can't start the hmi buttons thread, windows restart "
                       "buttons inline\n");
                jlog("rawplay: can't start the hmi buttons thread");
            }
        }
    }

    while (running) {
        unsigned type, len, seq, ts;
        int r;
        long long now;

        /* the layer helper attaches in the background after a window; until it is ready
         * (and the buttons service is stopped again) frames stay discarded so the hmi
         * keeps drawing and audio keeps playing. a new deadline opens a window even while
         * that return is pending: hmi_window_open() throws the half-attached layer away. */
        if (hmi_video_pending) {
            if (!hmi_video_ready) {
                int vr = panel_layer_resume_poll();

                if (vr == 1) {
                    hmi_video_ready = 1;
                } else if (vr < 0) {
                    printf("rawplay: no video layer after the hmi window, stopping the hmi\n");
                    jlog("rawplay: no video layer after the hmi window, stopping the hmi");
                    panel_layer_resume_abort();
                    hmi_video_pending = 0;
                    hmi_video_ready = 0;
                    if (fb_direct) {
                        fb.pixels = fb_direct;
                    }
                    layer_video = 0;
                    if (opt_hmi) {
                        hmi_signal(HMI_SIGSTOP);
                    }
                    hmi_window_log_close();
                }
            }
            if (hmi_video_ready && !hmi_buttons_busy()) {
                hmi_video_pending = 0;
                hmi_video_ready = 0;
                jlog("rawplay: video layer back after the hmi window");
                hmi_window_log_close();
            }
        }
        if (hmi_active && hmi_window_done()) {
            hmi_window_close();
        } else if (!hmi_active && hmi_until_ms &&
                   ((long)(hmi_until_ms - now_ms()) > 0 || reverse_hold)) {
            hmi_window_open();
        }
        if (hmi_active && hmi_resume_pending && !hmi_buttons_busy()) {
            hmi_signal(HMI_SIGCONT);
            hmi_resume_pending = 0;
        }
        r = read_header(&type, &len, &seq, &ts);
        now = now_us();

        if (r < 0) {
            if (raw_usb_error()) {
                /* the transport lost part of a transfer while scanning: no in-band
                 * resync exists, so drain it and look for the next header */
                printf("rawplay: bulk-in error, resyncing\n");
                jlog("rawplay: bulk-in error, resyncing");
                resync_stream();
                continue;
            }
            /* the transport distinguishes a quiet link from a removed device; the unit
             * has no console, so this is also the line that lands in the stick log */
            exit_reason = raw_usb_quiet()
                              ? (rx_bytes ? "stream stopped" : "no video from the host")
                              : "link read failed";
            printf("rawplay: %s\n", exit_reason);
            jlog("rawplay: %s (%llu kb, %lu frames)", exit_reason, rx_bytes / 1024, frames);
            break;
        }
        if (r == 2) {
            /* a candidate that failed the header check; keep scanning */
        } else if (r == 0) {
            if (now - last_rx_us > (rx_bytes ? LINK_DEAD_US : STARTUP_DEAD_US)) {
                exit_reason = rx_bytes ? "stream stopped" : "no video from the host";
                printf("rawplay: %s\n", exit_reason);
                jlog("rawplay: %s (%llu kb, %lu frames)", exit_reason, rx_bytes / 1024,
                     frames);
                break;
            }
        } else if (type == LR_MODE) {
            if (read_mode() < 0) {
                exit_reason = "host mode does not match the panel";
                break;
            }
        } else if (type == LR_AUDIO_CFG) {
            if (read_audio_cfg() < 0) {
                if (raw_usb_error()) {
                    printf("rawplay: bulk-in error in audio config, resyncing\n");
                    resync_stream();
                    continue;
                }
                exit_reason = "link read failed in audio config";
                break;
            }
            last_rx_us = now_us();
        } else if (type == LR_AUDIO) {
            if (read_audio(len) < 0) {
                if (raw_usb_error()) {
                    printf("rawplay: bulk-in error in audio, resyncing\n");
                    resync_stream();
                    continue;
                }
                exit_reason = "link read failed in audio";
                break;
            }
            last_rx_us = now_us();
        } else if (type == LR_BANDS) {
            long long t0 = now_us();
            int susp = video_suspended();
            int bad = susp ? (skip_pad(LR_MSG - LR_HEADER) < 0 || discard((int)len) < 0)
                           : read_bands(len) < 0;

            if (bad) {
                if (raw_usb_error()) {
                    printf("rawplay: bulk-in error in bands, dropped update %u\n", seq);
                    jlog("rawplay: bulk-in error in bands, dropped update %u", seq);
                    send_ack(seq, 2);
                    resync_stream();
                    continue;
                }
                exit_reason = "link read failed in bands";
                break;
            }
            rx_us += now_us() - t0;
            rx_bytes += len;
            last_rx_us = now_us();
            frames++;
            if (susp) {
                superseded++;       /* an HMI window: received, not painted */
                send_ack(seq, 2);
            } else {
                drawn++;
                send_ack(seq, 1);
            }
        } else if (type == LR_FRAME) {
            long long t0 = now_us();
            int susp = video_suspended();

            if (susp) {
                /* the hmi owns the panel: keep the pipe drained (audio is multiplexed
                 * into it), but never dma a picture into the hmi's framebuffer. the
                 * stale ack advances the host's window so the next audio chunk follows
                 * immediately instead of after a two-frame stall. */
                if (skip_pad(LR_MSG - LR_HEADER) < 0) {
                    if (raw_usb_error()) {
                        printf("rawplay: bulk-in error in frame padding, dropped frame %u\n",
                               seq);
                        send_ack(seq, 2);
                        resync_stream();
                        continue;
                    }
                    exit_reason = "link read failed mid frame";
                    printf("rawplay: %s\n", exit_reason);
                    jlog("rawplay: %s (%llu kb, %lu frames)", exit_reason, rx_bytes / 1024,
                         frames);
                    break;
                }
                if (len == (unsigned)expected_len) {
                    if (discard((int)len) < 0) {
                        if (raw_usb_error()) {
                            printf("rawplay: bulk-in error mid frame, dropped frame %u\n", seq);
                            send_ack(seq, 2);
                            resync_stream();
                            continue;
                        }
                        exit_reason = "link read failed mid frame";
                        printf("rawplay: %s\n", exit_reason);
                        jlog("rawplay: %s (%llu kb, %lu frames)", exit_reason,
                             rx_bytes / 1024, frames);
                        break;
                    }
                    rx_us += now_us() - t0;
                    rx_bytes += len;
                    last_rx_us = now_us();
                    frames++;
                    superseded++;
                } else if (discard((int)len) < 0) {
                    printf("rawplay: bulk-in error while discarding, resyncing\n");
                    resync_stream();
                    continue;
                }
                /* a wrong-sized frame is stale, exactly as in the paint path */
                send_ack(seq, 2);
                continue;
            }
            /* the header's ts is the host's send time, so consecutive deltas are the
             * source's cadence. printing it next to the guest fps makes clear when the
             * panel is showing every frame the host sends under the icount clock. */
            if (have_ts) {
                unsigned dt = ts - last_ts;

                if (dt) {
                    src_dt_us += (unsigned long long)dt * 1000;
                    src_frames++;
                }
            }
            last_ts = ts;
            have_ts = 1;
            if (skip_pad(LR_MSG - LR_HEADER) < 0) {
                if (raw_usb_error()) {
                    printf("rawplay: bulk-in error in frame padding, dropped frame %u\n", seq);
                    send_ack(seq, 2);
                    resync_stream();
                    continue;
                }
                exit_reason = "link read failed mid frame";
                printf("rawplay: %s\n", exit_reason);
                jlog("rawplay: %s (%llu kb, %lu frames)", exit_reason, rx_bytes / 1024,
                     frames);
                break;
            }
            if (len != (unsigned)expected_len) {
                printf("rawplay: frame %u bytes, expected %d, skipping\n", len, expected_len);
                if (discard(len) < 0) {
                    printf("rawplay: bulk-in error while discarding, resyncing\n");
                    resync_stream();
                    continue;
                }
                send_ack(seq, 2);
                continue;
            }
            /* a header already sitting in the refill buffer means this frame is already
             * old news: consume it without painting it (newest-wins). an audio message
             * between frames is not a superseding frame, so only a complete-looking
             * FRAME header counts here. */
            if (rxlen - rxpos >= LR_HEADER && rxbuf[rxpos] == LR_MAGIC0 &&
                rxbuf[rxpos + 1] == LR_MAGIC1 &&
                ((rxbuf[rxpos + 2] == LR_FRAME &&
                  rd32le(rxbuf + rxpos + 4) == (unsigned)expected_len) ||
                 rxbuf[rxpos + 2] == LR_BANDS)) {
                if (discard(len) < 0) {
                    printf("rawplay: bulk-in error while discarding, resyncing\n");
                    resync_stream();
                    continue;
                }
                superseded++;
                send_ack(seq, 2);
                continue;
            }
            if (read_exact_panel((unsigned char *)fb.pixels, (int)len) < 0) {
                if (raw_usb_error()) {
                    /* the frame is lost mid-transfer. ack it stale so the host's window
                     * moves on, resync, and take the next header. the panel holds part
                     * of the frame; the next one overwrites it */
                    printf("rawplay: bulk-in error mid frame, dropped frame %u\n", seq);
                    jlog("rawplay: bulk-in error mid frame, dropped frame %u, %llu kb, "
                         "%lu frames", seq, rx_bytes / 1024, frames);
                    send_ack(seq, 2);
                    resync_stream();
                    continue;
                }
                exit_reason = "link read failed mid frame";
                printf("rawplay: %s\n", exit_reason);
                jlog("rawplay: %s (%llu kb, %lu frames)", exit_reason, rx_bytes / 1024,
                     frames);
                break;
            }
            rx_us += now_us() - t0;
            rx_bytes += len;
            last_rx_us = now_us();
            frames++;
            drawn++;
            send_ack(seq, 1);
        } else {
            discard((int)len);
        }

        if (opt_seconds && now_us() - t_start >= (long long)opt_seconds * 1000000) {
            break;
        }
        if (now_us() - last_ready >= READY_INTERVAL_US) {
            last_ready = now_us();
            send_ready();
        }
        if (opt_stats && now_us() - last_stats >= 5000000) {
            long long el = now_us() - t_start;

            last_stats = now_us();
            printf("rawplay: %lu frames, %lu drawn, %lu stale, %llu kb, %.1f fps "
                   "(source %.1f), %.1f ms/frame, %s%s, audio %lu out/%lu mic, "
                   "under %lu, ring %lu, err %lu, drop %lu/%lu/%lu\n", frames, drawn,
                   superseded, rx_bytes / 1024, frames * 1000000.0 / el,
                   src_dt_us ? src_frames * 1000000.0 / src_dt_us : 0.0,
                   frames ? rx_us / 1000.0 / frames : 0.0,
                   raw_usb_direct() ? "direct" : "staged",
                   raw_usb_compacted() ? " (compacted)" : "",
                   aout.chunks, ain.chunks, aout.underruns, audio_level(), aout.errors,
                   dropped_in, dropped_ack, mic_drops);
            src_dt_us = 0;
            src_frames = 0;
        }
    }
    running = 0;
    audio_out_stop();
    mic_stop();
    if (hmi_active) {
        /* the exit screen is drawn where the hmi was; a failure message would otherwise be
         * painted over by a bar the hmi still owns. the window close also stops any
         * buttons restarted for it before the hmi goes down */
        hmi_window_close();
    }
    {
        long long el = now_us() - t_start;

        panel_clear(fb_rgb(10, 14, 24));
        con_init(&con, &fb);
        if (exit_reason) {
            con_printf(&con, "rawplay: %s\n", exit_reason);
        }
        printf("rawplay: %lu frames, %lu drawn, %lu stale, %llu kb in %lld ms (%.1f fps, "
               "%.1f ms/frame), %s%s, source %.1f fps, audio %lu out/%lu mic, "
               "under %lu, drop in/ack/mic %lu/%lu/%lu%s%s\n", frames,
               drawn, superseded, rx_bytes / 1024, el / 1000, frames * 1000000.0 / el,
               frames ? rx_us / 1000.0 / frames : 0.0, raw_usb_direct() ? "direct" : "staged",
               raw_usb_compacted() ? " (compacted)" : "",
               src_dt_us ? src_frames * 1000000.0 / src_dt_us : 0.0,
               aout.chunks, ain.chunks, aout.underruns, dropped_in, dropped_ack, mic_drops,
               exit_reason ? ", " : "", exit_reason ? exit_reason : "");
        jlog("rawplay: exit, %lu frames, %lu drawn, %llu kb, %s%s%s%s", frames, drawn,
             rx_bytes / 1024, raw_usb_direct() ? "direct" : "staged",
             raw_usb_compacted() ? " (compacted)" : "", exit_reason ? ", " : "",
             exit_reason ? exit_reason : "");
        /* a failure would otherwise flash past unread on a unit with no console. the
         * video layer stays up for the message and is dropped afterwards, which hands
         * the panel back to the hmi without ever stopping it. */
        if (exit_reason) {
            usleep(1000000);
        }
    }
    panel_finish();
    return 0;
}
