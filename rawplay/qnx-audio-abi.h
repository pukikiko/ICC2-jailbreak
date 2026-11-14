/*
 * just enough of QNX's sys/asound.h for the PCM plugin API rawplay uses.
 *
 * the structures are copied from the QNX 6.x public audio headers (Apache-2.0); the
 * comments about reserved fields matter: io-audio checks them, so every one is zeroed.
 * rawplay links a tiny link-time stub and the unit's real /lib/libasound.so.2 is loaded
 * at run time, exactly like the libgf/libusbdi stubs the rest of the sdk uses.
 */
#ifndef QNX_AUDIO_ABI_H
#define QNX_AUDIO_ABI_H

#include "qnx.h"

typedef unsigned char uint8_t;
typedef signed char int8_t;
typedef unsigned short uint16_t;
typedef short int16_t;
typedef unsigned int uint32_t;
typedef int int32_t;
typedef unsigned long long uint64_t;
typedef long long int64_t;

/* open modes (sys/asoundlib.h) */
#define SND_PCM_OPEN_PLAYBACK 0x0001
#define SND_PCM_OPEN_CAPTURE  0x0002
#define SND_PCM_OPEN_NONBLOCK 0x1000

/* channel directions (sys/asound_common.h) */
#define SND_PCM_CHANNEL_PLAYBACK 0
#define SND_PCM_CHANNEL_CAPTURE  1

/* transfer modes */
#define SND_PCM_MODE_BLOCK 1

/* sample formats */
#define SND_PCM_SFMT_U8     0
#define SND_PCM_SFMT_S16_LE 4

/* start/stop modes */
#define SND_PCM_START_DATA   1
#define SND_PCM_START_FULL   2
#define SND_PCM_START_GO     3
#define SND_PCM_STOP_STOP    1
#define SND_PCM_STOP_ROLLOVER 2

/* channel status values */
#define SND_PCM_STATUS_READY     1
#define SND_PCM_STATUS_PREPARED  2
#define SND_PCM_STATUS_RUNNING   3
#define SND_PCM_STATUS_UNDERRUN  4
#define SND_PCM_STATUS_OVERRUN   5

typedef union snd_pcm_sync {
    uint8_t id[16];
    uint16_t id16[8];
    uint32_t id32[4];
    uint64_t id64[2];
} snd_pcm_sync_t;

typedef struct snd_pcm_digital {
    uint8_t dig_status[24];
    uint8_t dig_subcode[147];
    uint8_t dig_valid:1;
    uint8_t dig_subframe[4];
    uint8_t reserved[128];
} snd_pcm_digital_t;

typedef struct snd_pcm_format {
    uint32_t interleave:1;
    int32_t format;
    int32_t rate;
    int32_t voices;
    int32_t special;
    uint8_t reserved[124];
} snd_pcm_format_t;

typedef struct snd_pcm_channel_params {
    int32_t channel;
    int32_t mode;
    snd_pcm_sync_t sync;
    struct snd_pcm_format format;
    snd_pcm_digital_t digital;
    int32_t start_mode;
    int32_t stop_mode;
    int32_t time:1, ust_time:1;
    uint32_t why_failed;
    union {
        struct {
            int32_t queue_size;
            int32_t fill;
            int32_t max_fill;
            uint8_t reserved[124];
        } stream;
        struct {
            int32_t frag_size;
            int32_t frags_min;
            int32_t frags_max;
            uint32_t frags_buffered_max;
            uint8_t reserved[120];
        } block;
        uint8_t reserved[128];
    } buf;
    char sw_mixer_subchn_name[32];
    uint32_t cross_core_handle;
    uint8_t reserved[92];
} snd_pcm_channel_params_t;

/* only the leading fields are used; the mixer pointers are the same width as void * */
typedef struct snd_pcm_channel_setup {
    int32_t channel;
    int32_t mode;
    struct snd_pcm_format format;
    snd_pcm_digital_t digital;
    union {
        struct {
            int32_t queue_size;
            uint8_t reserved[124];
        } stream;
        struct {
            int32_t frag_size;
            int32_t frags;
            int32_t frags_min;
            int32_t frags_max;
            uint32_t max_frag_size;
            uint8_t reserved[124];
        } block;
        uint8_t reserved[128];
    } buf;
    int16_t msbits_per_sample;
    int16_t pad1;
    int32_t mixer_device;
    void *mixer_eid;
    void *mixer_gid;
    uint8_t mmap_valid:1;
    uint8_t mmap_active:1;
    int32_t mixer_card;
    uint8_t reserved[104];
} snd_pcm_channel_setup_t;

typedef struct snd_pcm_channel_status {
    int32_t channel;
    int32_t mode;
    int32_t status;
    uint32_t scount;
    uint32_t stime_sec;
    uint32_t stime_usec;
    uint64_t ust_stime;
    int32_t frag;
    int32_t count;
    int32_t free_count;
    int32_t underrun;
    int32_t overrun;
    int32_t overrange;
    uint32_t subbuffered;
    uint8_t status_data[4];
    uint32_t stop_sec;
    uint32_t stop_usec;
    uint8_t hw_device;
    uint8_t reserved[111];
} snd_pcm_channel_status_t;

int snd_pcm_open_preferred(void **handle, int *rcard, int *rdevice, int mode);
int snd_pcm_open(void **handle, int card, int device, int mode);
int snd_pcm_close(void *handle);
int snd_pcm_nonblock_mode(void *handle, int nonblock);
int snd_pcm_channel_params(void *handle, snd_pcm_channel_params_t *params);
int snd_pcm_channel_setup(void *handle, snd_pcm_channel_setup_t *setup);
int snd_pcm_channel_prepare(void *handle, int channel);
int snd_pcm_channel_go(void *handle, int channel);
int snd_pcm_capture_go(void *handle);
int snd_pcm_channel_status(void *handle, snd_pcm_channel_status_t *status);
unsigned int snd_pcm_plugin_set_disable(void *handle, unsigned int mask);
int snd_pcm_plugin_params(void *handle, snd_pcm_channel_params_t *params);
int snd_pcm_plugin_setup(void *handle, snd_pcm_channel_setup_t *setup);
int snd_pcm_plugin_prepare(void *handle, int channel);
int snd_pcm_plugin_flush(void *handle, int channel);
int snd_pcm_plugin_playback_drain(void *handle);
ssize_t snd_pcm_plugin_write(void *handle, const void *buffer, size_t size);
ssize_t snd_pcm_plugin_read(void *handle, void *buffer, size_t size);
ssize_t snd_pcm_write(void *handle, const void *buffer, size_t size);
ssize_t snd_pcm_read(void *handle, void *buffer, size_t size);

/* the driver reads this struct directly; a wrong size here is garbage parameters */
typedef char qnx_audio_params_size_check[
    sizeof(snd_pcm_channel_params_t) == 752 ? 1 : -1];
typedef char qnx_audio_format_size_check[
    sizeof(snd_pcm_format_t) == 144 ? 1 : -1];

#endif
