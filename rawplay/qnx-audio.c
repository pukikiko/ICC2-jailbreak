/*
 * QNX PCM playback/capture through the audio plugin API. the plugin takes care of rate,
 * format and voice conversion, so rawplay always asks for the wire formats (44100/s16le/2
 * out, 8000/u8/1 in) and lets io-audio adapt to the codec.
 */
#include "qnx.h"
#include "qnx-audio.h"
#include "qnx-audio-abi.h"

static int pcm_open(struct qa_pcm *p, int mode, int card, int device, int rate,
                    int channels, int format, int frag_bytes, int plugin)
{
    snd_pcm_channel_params_t params;
    snd_pcm_channel_setup_t setup;
    int opened_card = -1, opened_device = -1;
    int r;

    memset(p, 0, sizeof *p);
    p->handle = 0;
    p->channel = mode == SND_PCM_OPEN_PLAYBACK ? SND_PCM_CHANNEL_PLAYBACK
                                               : SND_PCM_CHANNEL_CAPTURE;
    p->rate = rate;
    p->channels = channels;
    p->format = format;
    p->frag = frag_bytes;
    p->plugin = plugin;

    if (card >= 0) {
        r = snd_pcm_open(&p->handle, card, device, mode | SND_PCM_OPEN_NONBLOCK);
    } else {
        r = snd_pcm_open_preferred(&p->handle, &opened_card, &opened_device,
                                   mode | SND_PCM_OPEN_NONBLOCK);
    }
    if (r < 0 || !p->handle) {
        p->handle = 0;
        return -1;
    }
    p->card = card >= 0 ? card : opened_card;
    p->device = card >= 0 ? device : opened_device;
    (void)snd_pcm_nonblock_mode(p->handle, 1);

    memset(&params, 0, sizeof params);
    params.channel = p->channel;
    params.mode = SND_PCM_MODE_BLOCK;
    params.format.interleave = 1;
    params.format.format = format;
    params.format.rate = rate;
    params.format.voices = channels;
    params.start_mode = mode == SND_PCM_OPEN_PLAYBACK ? SND_PCM_START_FULL
                                                       : SND_PCM_START_DATA;
    params.stop_mode = SND_PCM_STOP_STOP;
    params.buf.block.frag_size = frag_bytes;
    params.buf.block.frags_min = 1;
    /* playback asks for two wire-sized fragments (40 ms of codec ring): enough for the
     * driver's own scheduling, half the old 80 ms. the application prebuffer above it
     * absorbs the link jitter. capture keeps the driver's choice. */
    params.buf.block.frags_max = mode == SND_PCM_OPEN_PLAYBACK ? 2 : -1;

    if (mode == SND_PCM_OPEN_PLAYBACK) {
        /* the unit's audio manager is happier with a single-fragment ring and no mmap;
         * this is the shape the qnx wave example uses for a plain playback stream */
        snd_pcm_plugin_set_disable(p->handle, 3 /* partial blocks + mmap */);
    }

    r = plugin ? snd_pcm_plugin_params(p->handle, &params)
               : snd_pcm_channel_params(p->handle, &params);
    if (r < 0) {
        /* some drivers reject a requested fragment outright; let the plugin choose one
         * and keep the format/rate/voices it accepted above. */
        params.buf.block.frag_size = 0;
        r = plugin ? snd_pcm_plugin_params(p->handle, &params)
                   : snd_pcm_channel_params(p->handle, &params);
    }
    if (r < 0) {
        snd_pcm_close(p->handle);
        p->handle = 0;
        return -1;
    }

    memset(&setup, 0, sizeof setup);
    setup.channel = p->channel;
    r = plugin ? snd_pcm_plugin_setup(p->handle, &setup)
               : snd_pcm_channel_setup(p->handle, &setup);
    if (r == 0 && setup.buf.block.frag_size > 0) {
        p->frag = setup.buf.block.frag_size;
    }
    r = plugin ? snd_pcm_plugin_prepare(p->handle, p->channel)
               : snd_pcm_channel_prepare(p->handle, p->channel);
    if (r < 0) {
        snd_pcm_close(p->handle);
        p->handle = 0;
        return -1;
    }
    if (p->channel == SND_PCM_CHANNEL_CAPTURE) {
        (void)snd_pcm_channel_go(p->handle, p->channel);
    }
    p->open = 1;
    return 0;
}

int qa_open_playback(struct qa_pcm *p, int rate, int channels, int frag_bytes)
{
    /* playback is the codec's native 44.1k s16 stereo, so the non-plugin interface can
     * take whole fragments with no partial-block buffering in the way */
    return pcm_open(p, SND_PCM_OPEN_PLAYBACK, -1, -1, rate, channels,
                    SND_PCM_SFMT_S16_LE, frag_bytes, 0);
}

int qa_open_playback_at(struct qa_pcm *p, int card, int device, int rate, int channels,
                        int frag_bytes)
{
    return pcm_open(p, SND_PCM_OPEN_PLAYBACK, card, device, rate, channels,
                    SND_PCM_SFMT_S16_LE, frag_bytes, 0);
}

int qa_open_capture(struct qa_pcm *p, int rate, int channels, int frag_bytes)
{
    /* capture asks for u8/8k mono; the plugin does the rate/format conversion */
    return pcm_open(p, SND_PCM_OPEN_CAPTURE, -1, -1, rate, channels, SND_PCM_SFMT_U8,
                    frag_bytes, 1);
}

int qa_open_capture_native(struct qa_pcm *p, int rate, int channels, int frag_bytes)
{
    /* the codec's own capture format: 16-bit stereo at 44.1/48 kHz. the plugin is used
     * for the fragment plumbing, but there is no format/rate conversion; rawplay does the
     * mono mix and 8 kHz resample itself. */
    return pcm_open(p, SND_PCM_OPEN_CAPTURE, -1, -1, rate, channels,
                    SND_PCM_SFMT_S16_LE, frag_bytes, 1);
}

int qa_open_capture_at(struct qa_pcm *p, int card, int device, int rate, int channels,
                       int frag_bytes)
{
    return pcm_open(p, SND_PCM_OPEN_CAPTURE, card, device, rate, channels,
                    SND_PCM_SFMT_S16_LE, frag_bytes, 1);
}

void qa_close(struct qa_pcm *p)
{
    if (!p->handle) {
        return;
    }
    /* playback uses the non-plugin handle; a plugin flush there can block behind a
     * driver that stopped consuming (seen with qemu's wav backend on an underrun), and
     * the close below already stops the stream. only the plugin capture path flushes. */
    if (p->channel == SND_PCM_CHANNEL_PLAYBACK && p->plugin) {
        (void)snd_pcm_plugin_flush(p->handle, p->channel);
    }
    snd_pcm_close(p->handle);
    p->handle = 0;
    p->open = 0;
}

int qa_prepare(struct qa_pcm *p)
{
    int r;

    if (!p->handle) {
        return -1;
    }
    r = p->plugin ? snd_pcm_plugin_prepare(p->handle, p->channel)
                  : snd_pcm_channel_prepare(p->handle, p->channel);
    if (r == 0) {
        (void)snd_pcm_channel_go(p->handle, p->channel);
    }
    return r;
}

/* a playback stream that ran dry is in UNDERRUN and the plugin returns EAGAIN instead of
 * an error; reprepare it, exactly what the qnx wave example does on a short write. */
static int qa_recover(struct qa_pcm *p)
{
    snd_pcm_channel_status_t st;
    int r;

    memset(&st, 0, sizeof st);
    st.channel = p->channel;
    r = snd_pcm_channel_status(p->handle, &st);
    if (r < 0) {
        return -1;
    }
    if (st.status == SND_PCM_STATUS_UNDERRUN || st.status == SND_PCM_STATUS_OVERRUN ||
        st.status == SND_PCM_STATUS_READY || st.status == SND_PCM_STATUS_PREPARED) {
        return qa_prepare(p);
    }
    return 0;                   /* still running: the buffer will free a fragment */
}

int qa_write(struct qa_pcm *p, const void *buf, int len)
{
    const unsigned char *q = buf;
    int done = 0;
    int retried = 0, stalls = 0;

    if (!p->handle) {
        return -1;
    }
    while (done < len) {
        ssize_t n = p->plugin
                        ? snd_pcm_plugin_write(p->handle, q + done, (size_t)(len - done))
                        : snd_pcm_write(p->handle, q + done, (size_t)(len - done));

        if (n > 0) {
            done += (int)n;
            retried = 0;
            stalls = 0;
            continue;
        }
        p->last_error = (int)n;
        if (n == 0 || n == -11) {       /* -EAGAIN/-EWOULDBLOCK: the plugin buffer is full */
            if (p->stop && *p->stop) {
                return -1;
            }
            if ((stalls++ % 10) == 0 && qa_recover(p) < 0) {
                return -1;
            }
            if (stalls > 1000) {
                return -1;
            }
            usleep(2000);
            continue;
        }
        if (!retried && qa_recover(p) == 0) {
            retried = 1;
            continue;
        }
        return -1;
    }
    return 0;
}

int qa_read(struct qa_pcm *p, void *buf, int len)
{
    int done = 0;
    int retried = 0, stalls = 0;

    if (!p->handle) {
        return -1;
    }
    while (done < len) {
        ssize_t n = p->plugin
                        ? snd_pcm_plugin_read(p->handle, (unsigned char *)buf + done,
                                              (size_t)(len - done))
                        : snd_pcm_read(p->handle, (unsigned char *)buf + done,
                                       (size_t)(len - done));

        if (n > 0) {
            done += (int)n;
            retried = 0;
            stalls = 0;
            continue;
        }
        p->last_error = (int)n;
        if (n == 0 || n == -11) {
            if (p->stop && *p->stop) {
                return done > 0 ? done : -1;
            }
            if (++stalls > 1000) {
                return done > 0 ? done : -1;
            }
            usleep(1000);
            continue;
        }
        if (!retried && qa_recover(p) == 0) {
            retried = 1;
            continue;
        }
        return done > 0 ? done : -1;
    }
    return done;
}
