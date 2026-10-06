/* C ABI for the FAK lossless audio codec (capi, Rust staticlib).
 *
 * Ownership: fak_decoder_open copies the input bytes (fak_decoder_open_cb keeps none); every pointer returned by a fak_decoder_*
 * getter stays valid until fak_decoder_free. Buffers from fak_rewrite_metadata are released with
 * fak_buffer_free. A handle may be used from one thread at a time. No function unwinds or aborts
 * on malformed input; failures return null / -1 with a message (err buffer or last_error). */
#ifndef FAK_CAPI_H
#define FAK_CAPI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FakDecoder FakDecoder;

typedef struct FakInfo {
    uint32_t channels;
    uint32_t bits_per_sample;  /* 8, 16, 24 or 32 */
    uint32_t sample_rate;
    uint32_t mode;             /* 0 = block-independent (the only mode since format v19) */
    uint64_t total_frames;
    uint64_t chunk_count;
    uint32_t format_version;
    uint32_t has_channel_mask;
    uint32_t channel_mask;     /* WAVE_FORMAT_EXTENSIBLE dwChannelMask, if has_channel_mask */
    uint32_t is_float;         /* 1: 32-bit float stream, decode with fak_decoder_decode_chunk_float;
                                  bits_per_sample is then the internal integer mapping's width */
    uint32_t chunk_frames;     /* sample-frames in a full chunk (the last one may be shorter) */
    uint32_t fec_group;        /* data chunks per FEC parity block (the largest block); 0 = no error-recovery data */
    uint32_t parity_blocks;    /* number of FEC parity blocks */
    uint8_t pcm_sha256[32];
} FakInfo;

typedef struct FakPicture {
    uint32_t kind;             /* FLAC/ID3 picture type: 3 front, 4 back, 8 artist, ... */
    const char *mime;
    const char *description;
    const uint8_t *data;
    size_t data_len;
} FakPicture;

typedef struct FakPictureIn {
    uint32_t kind;
    const char *mime;
    const char *description;
    const uint8_t *data;
    size_t data_len;
} FakPictureIn;

typedef struct FakBuffer {
    uint8_t *data;
    size_t len;
} FakBuffer;

FakDecoder *fak_decoder_open(const uint8_t *data, size_t len, char *err, size_t err_cap);

/* Reads len bytes at absolute position pos into buf. Returns 0 on success, non-zero on failure. Must
 * not throw or unwind: catch everything inside and return non-zero. */
typedef int (*fak_read_fn)(void *user, uint64_t pos, uint8_t *buf, size_t len);

/* Like fak_decoder_open but the file stays with the host: the library reads the header, metadata and
 * a small chunk index through `read`, then one chunk at a time while decoding, and keeps no copy of
 * the file (about 8 MB resident for 24-bit/96 kHz stereo, whatever the file size). `user` and `read`
 * must stay valid until fak_decoder_free. Only one call on the decoder at a time. */
FakDecoder *fak_decoder_open_cb(fak_read_fn read, void *user, uint64_t size, char *err, size_t err_cap);
void fak_decoder_free(FakDecoder *d);
const char *fak_decoder_last_error(const FakDecoder *d);
int fak_decoder_info(const FakDecoder *d, FakInfo *out);

int64_t fak_decoder_chunk_for_frame(const FakDecoder *d, uint64_t frame); /* -1 past the end */
uint64_t fak_decoder_chunk_start(const FakDecoder *d, uint64_t chunk);
uint64_t fak_decoder_chunk_frames(const FakDecoder *d, uint64_t chunk);
/* Interleaved signed samples at the stream's own scale; out_cap in samples. Returns frames or -1. */
int64_t fak_decoder_decode_chunk(FakDecoder *d, uint64_t chunk, int32_t *out, size_t out_cap);

/* Float streams (FakInfo.is_float): interleaved float32, bit-exact to the source; out_cap in samples. */
int64_t fak_decoder_decode_chunk_float(FakDecoder *d, uint64_t chunk, float *out, size_t out_cap);

const char *fak_decoder_vendor(const FakDecoder *d);
size_t fak_decoder_tag_count(const FakDecoder *d);
const char *fak_decoder_tag(const FakDecoder *d, size_t i); /* "KEY=VALUE", UTF-8 */
size_t fak_decoder_picture_count(const FakDecoder *d);
int fak_decoder_picture(const FakDecoder *d, size_t i, FakPicture *out);
/* Cue sheet as .cue text (the CUESHEET tag, or generated from the binary cue sheet); NULL if none. */
const char *fak_decoder_cuesheet(const FakDecoder *d);

/* New file bytes with tags (and optionally pictures) replaced; audio untouched. 0 on success.
 * The binary cue sheet follows a CUESHEET tag in `tags` (parsed and validated; a bad sheet fails
 * the call); without one it is dropped, unless the file only ever had the binary form. */
int fak_rewrite_metadata(FakDecoder *d, const char *const *tags, size_t tag_count,
                         int replace_pictures, const FakPictureIn *pictures, size_t picture_count,
                         FakBuffer *out);
void fak_buffer_free(FakBuffer b);


/* ---- Encoding ------------------------------------------------------------------------------ */
typedef struct FakEncoder FakEncoder;
/* Called from the encoding thread after each batch: frames done of total. Non-zero cancels. */
typedef int (*FakProgress)(void *user, uint64_t frames_done, uint64_t frames_total);

/* bits_per_sample 8, 16, 24 or 32; NULL if unsupported. expected_frames is only a capacity hint. */
FakEncoder *fak_encoder_new(uint32_t channels, uint32_t bits_per_sample, uint32_t sample_rate, uint64_t expected_frames);
void fak_encoder_free(FakEncoder *e);
/* 32-bit float PCM, stored losslessly (any bit pattern, NaN and infinities included). */
FakEncoder *fak_encoder_new_float(uint32_t channels, uint32_t sample_rate, uint64_t expected_frames);
int fak_encoder_push_float(FakEncoder *e, const float *interleaved, size_t frames);
const char *fak_encoder_last_error(const FakEncoder *e);
/* Speaker mask to store (WAVE_FORMAT_EXTENSIBLE); 0 or a mask whose popcount != channels stores none. */
void fak_encoder_set_channel_mask(FakEncoder *e, uint32_t mask);
/* Interleaved samples at the stream's own scale (16-bit: -32768..32767). 0, or -1 if out of range. */
int fak_encoder_push(FakEncoder *e, const int32_t *interleaved, size_t frames);
/* Writes len bytes at absolute position pos of the output file; returns 0 on success, non-zero on
 * failure. Must not throw or unwind. The library also rewrites the header at position 0 at the end. */
typedef int (*fak_write_fn)(void *user, uint64_t pos, const uint8_t *buf, size_t len);

/* Makes an encoder from fak_encoder_new write straight to a file as audio is pushed, so memory stays
 * at a few chunks instead of the whole track (a 275 MB WAV needed 733 MB). Metadata (tags, pictures)
 * is written first, each chunk as it fills, and fak_encoder_end_stream writes the real header. Level,
 * threads, chunk_seconds and fec_group are as for fak_encoder_finish. Call before the first push;
 * expected_frames is only used for progress. Not for float encoders or CD tags (both need all the
 * audio first: use fak_encoder_finish). Returns 0, or -1 (see fak_encoder_last_error). A non-zero
 * return from `progress` (checked after every chunk) cancels: the call in progress returns -1 with
 * "cancelled" and the partial file must be discarded. */
int fak_encoder_begin_stream(FakEncoder *e, fak_write_fn write, void *sink, int level, uint32_t threads,
                             uint32_t chunk_seconds, uint32_t fec_group, const char *const *tags, size_t tag_count,
                             const FakPictureIn *pictures, size_t picture_count, uint64_t expected_frames,
                             FakProgress progress, void *progress_user);

/* As fak_rewrite_metadata, but the new file goes through `write` (positions ascend from 0) and the audio is
 * copied in pieces, so memory does not grow with the file. 0 on success. */
int fak_rewrite_metadata_stream(FakDecoder *d, const char *const *tags, size_t tag_count,
                                int replace_pictures, const FakPictureIn *pictures, size_t picture_count,
                                fak_write_fn write, void *sink);

/* Encodes the last partial chunk and writes the final header; the file is complete after this. */
int fak_encoder_end_stream(FakEncoder *e);

/* level: 0 fast, 1 normal, 2 max, 3 insane (archival = level 3 + fec_group 0xFFFFFFFF). fec_group 0 = none,
 * 0xFFFFFFFF = Reed-Solomon parity over the whole file (about 1% of its chunks can be rebuilt), else
 * 1..60000: one Reed-Solomon parity block per that many data chunks. chunk_seconds 0 = automatic. threads 0 = automatic.
 * Tags are "KEY=VALUE"; a "CUESHEET=<cue text>" tag embeds a cue sheet. cd_tags != 0 adds the CD
 * identifiers and AccurateRip/CTDB checksums when the cue sheet and audio are an exact CD image (none
 * otherwise). The file's encoder string records the library versions and level/FEC/chunk settings.
 * Returns 0, or -1 (message in fak_encoder_last_error; "cancelled" if progress returned non-zero). */
int fak_encoder_finish(FakEncoder *e, int level, uint32_t threads, uint32_t chunk_seconds, uint32_t fec_group,
                       int cd_tags, const char *const *tags, size_t tag_count,
                       const FakPictureIn *pictures, size_t picture_count,
                       FakProgress progress, void *user, FakBuffer *out);

/* Decode fully and compare with the SHA-256 of the source PCM in the header. 0 = identical. */
int fak_verify(const uint8_t *data, size_t len, uint32_t threads, char *err, size_t err_cap);

/* fak_verify for a file that stays with the host (same read callback as fak_decoder_open_cb): chunks
 * are read, decoded and hashed in batches, so memory is a few chunks whatever the file size. */
int fak_verify_cb(fak_read_fn read, void *user, uint64_t size, uint32_t threads, char *err, size_t err_cap);

#ifdef __cplusplus
}
#endif
#endif
