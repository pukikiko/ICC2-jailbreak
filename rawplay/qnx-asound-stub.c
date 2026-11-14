/*
 * link-time stub for QNX's libasound.so.2. the unit runs the real library; this only
 * gives lld the symbols (the target library is stripped too aggressively to link).
 */
#include "qnx-audio-abi.h"

int snd_pcm_open_preferred(void **handle, int *rcard, int *rdevice, int mode)
{
    (void)handle; (void)rcard; (void)rdevice; (void)mode;
    return -1;
}

int snd_pcm_open(void **handle, int card, int device, int mode)
{
    (void)handle; (void)card; (void)device; (void)mode;
    return -1;
}

int snd_pcm_close(void *handle)
{
    (void)handle;
    return -1;
}

int snd_pcm_nonblock_mode(void *handle, int nonblock)
{
    (void)handle; (void)nonblock;
    return -1;
}

int snd_pcm_plugin_params(void *handle, snd_pcm_channel_params_t *params)
{
    (void)handle; (void)params;
    return -1;
}

int snd_pcm_channel_params(void *handle, snd_pcm_channel_params_t *params)
{
    (void)handle; (void)params;
    return -1;
}

int snd_pcm_channel_setup(void *handle, snd_pcm_channel_setup_t *setup)
{
    (void)handle; (void)setup;
    return -1;
}

int snd_pcm_channel_prepare(void *handle, int channel)
{
    (void)handle; (void)channel;
    return -1;
}

int snd_pcm_channel_go(void *handle, int channel)
{
    (void)handle; (void)channel;
    return -1;
}

int snd_pcm_capture_go(void *handle)
{
    (void)handle;
    return -1;
}

int snd_pcm_channel_status(void *handle, snd_pcm_channel_status_t *status)
{
    (void)handle; (void)status;
    return -1;
}

unsigned int snd_pcm_plugin_set_disable(void *handle, unsigned int mask)
{
    (void)handle; (void)mask;
    return 0;
}

ssize_t snd_pcm_write(void *handle, const void *buffer, size_t size)
{
    (void)handle; (void)buffer; (void)size;
    return -1;
}

ssize_t snd_pcm_read(void *handle, void *buffer, size_t size)
{
    (void)handle; (void)buffer; (void)size;
    return -1;
}

int snd_pcm_plugin_setup(void *handle, snd_pcm_channel_setup_t *setup)
{
    (void)handle; (void)setup;
    return -1;
}

int snd_pcm_plugin_prepare(void *handle, int channel)
{
    (void)handle; (void)channel;
    return -1;
}

int snd_pcm_plugin_flush(void *handle, int channel)
{
    (void)handle; (void)channel;
    return -1;
}

int snd_pcm_plugin_playback_drain(void *handle)
{
    (void)handle;
    return -1;
}

ssize_t snd_pcm_plugin_write(void *handle, const void *buffer, size_t size)
{
    (void)handle; (void)buffer; (void)size;
    return -1;
}

ssize_t snd_pcm_plugin_read(void *handle, void *buffer, size_t size)
{
    (void)handle; (void)buffer; (void)size;
    return -1;
}
