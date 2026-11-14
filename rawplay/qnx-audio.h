/*
 * rawplay's QNX PCM wrapper: open the preferred device, then the plugin API converts
 * between the wire format and whatever the codec accepts.
 */
#ifndef QNX_AUDIO_H
#define QNX_AUDIO_H

#include "qnx.h"

struct qa_pcm {
    void *handle;
    int channel;
    int rate;
    int channels;
    int format;
    int frag;
    int card;
    int device;
    int open;
    int plugin;
    int last_error;
    volatile int *stop;
};

int qa_open_playback(struct qa_pcm *p, int rate, int channels, int frag_bytes);
int qa_open_playback_at(struct qa_pcm *p, int card, int device, int rate, int channels,
                        int frag_bytes);
int qa_open_capture(struct qa_pcm *p, int rate, int channels, int frag_bytes);
int qa_open_capture_native(struct qa_pcm *p, int rate, int channels, int frag_bytes);
int qa_open_capture_at(struct qa_pcm *p, int card, int device, int rate, int channels,
                       int frag_bytes);

#define QA_FMT_U8     0
#define QA_FMT_S16LE  4
int qa_prepare(struct qa_pcm *p);
int qa_write(struct qa_pcm *p, const void *buf, int len);
int qa_read(struct qa_pcm *p, void *buf, int len);
void qa_close(struct qa_pcm *p);

#endif
