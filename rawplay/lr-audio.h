/*
 * Messages carried inside rawplay's LR/LI streams.
 *
 * downstream (host -> unit, bulk-in, interleaved with LR video):
 *   LR_AUDIO_CFG  payload 16 bytes: format, rate, channels, chunk_ms (u32 LE each)
 *   LR_AUDIO      payload: PCM samples, followed by zero padding to a 512-byte boundary
 *
 * upstream (unit -> host, bulk-out, interleaved with LI touch/acks):
 *   LI_AUDIO_CFG  12 bytes: 'L','I',type,ver,format,rate(u16),channels,chunk_ms,reserved[3]
 *   LI_AUDIO      13-byte header: 'L','I',type,len(u16),seq(u32),ts_ms(u32), then PCM
 *   LI_CAPS       8 bytes: 'L','I',type,ver,flags,reserved[3]
 *
 * this header is shared by the QNX unit, the Linux appliance and the PC test client; it
 * deliberately uses only preprocessor constants so it needs no libc headers.
 */
#ifndef LR_AUDIO_H
#define LR_AUDIO_H

/* LR types: MODE is 1, FRAME is 2 */
#define LR_AUDIO_CFG       3
#define LR_AUDIO           4
/* optional video encodings, only used after the unit advertises the matching LI_CAPS
 * bit. BANDS carries changed row runs as raw rgb565 straight into the scanout buffer. */
#define LR_BANDS           5

/* LI types: touch 1, knob 2, button 3, ready 5, raw ack 8 */
#define LI_AUDIO_CFG       9
#define LI_AUDIO           10
#define LI_CAPS            11

/* LI_CAPS flags. the message is fixed-size so a host that does not know the type can
 * skip its two magic bytes and resync without ever mistaking the payload for a header. */
#define LR_CAP_BANDS       0x01
#define LI_CAPS_LEN        8
#define LI_CAPS_VER        1

/* a BANDS payload starts with one maxpacket of map: u16 count, u16 reserved, then
 * count * { u16 y, u16 rows }. all row runs are multiples of 8 rows and their pixel
 * data follows the map in the same order. */
#define LR_BAND_MAP        512
#define LR_BAND_MAX        64
#define LR_BAND_ROW_ALIGN  8

/* sample formats in the config messages */
#define AUDIO_FMT_S16LE    1
#define AUDIO_FMT_U8       2

/* the formats this implementation speaks. the car side is 16-bit 44.1 kHz stereo;
 * the microphone uplink is 8-bit 8 kHz mono, as the ICC's voice path expects. */
#define AUDIO_OUT_FORMAT   AUDIO_FMT_S16LE
#define AUDIO_OUT_RATE     44100
#define AUDIO_OUT_CHANNELS 2
#define AUDIO_OUT_CHUNK_MS 20

#define AUDIO_IN_FORMAT    AUDIO_FMT_U8
#define AUDIO_IN_RATE      8000
#define AUDIO_IN_CHANNELS  1
#define AUDIO_IN_CHUNK_MS  20

/* fixed sizes for the default chunking (20 ms) */
#define AUDIO_OUT_CHUNK_BYTES \
    ((AUDIO_OUT_RATE * AUDIO_OUT_CHUNK_MS / 1000) * AUDIO_OUT_CHANNELS * 2)
#define AUDIO_IN_CHUNK_BYTES \
    ((AUDIO_IN_RATE * AUDIO_IN_CHUNK_MS / 1000) * AUDIO_IN_CHANNELS)

/* the playback device is fed in wire chunks (20 ms). the wire and the driver fragment stay
 * the same size; a two-fragment ring keeps the codec buffering at 40 ms, and the player's
 * application prebuffer above it absorbs the link jitter. a driver that refuses the size
 * falls back to its own fragment (qnx-audio.c reports it in `frag`). */
#define AUDIO_OUT_FRAG_MS    AUDIO_OUT_CHUNK_MS
#define AUDIO_OUT_FRAG_BYTES \
    ((AUDIO_OUT_RATE * AUDIO_OUT_FRAG_MS / 1000) * AUDIO_OUT_CHANNELS * 2)

#define LR_AUDIO_CFG_LEN   16
#define LI_AUDIO_CFG_LEN   12
#define LI_AUDIO_HDR_LEN   13

/* bounds so a corrupt length cannot turn into a giant read */
#define AUDIO_MAX_PAYLOAD  (64 * 1024)

#endif
