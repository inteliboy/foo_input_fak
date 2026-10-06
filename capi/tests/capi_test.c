/* Stand-alone test of the C ABI (fak_capi.h) as a C caller sees it -- struct layouts, ownership,
 * error paths -- against real files: decodes a .fak chunk by chunk and compares with the source
 * WAV's PCM; checks chunk lookup at every boundary; rewrites tags and pictures and checks the audio
 * is unchanged; feeds corrupted/truncated bytes. Build and run: capi/tests/run.ps1
 * usage: capi_test <file.fak> <source.wav> */
#include "fak_capi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

static unsigned char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END); *len = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc(*len ? *len : 1);
    if (fread(b, 1, *len, f) != *len) { printf("short read %s\n", path); exit(2); }
    fclose(f);
    return b;
}

/* Locates the WAV data chunk; returns its offset and sets *len. */
static size_t wav_data(const unsigned char *w, size_t n, size_t *len) {
    size_t pos = 12;
    while (pos + 8 <= n) {
        size_t sz = w[pos + 4] | (w[pos + 5] << 8) | (w[pos + 6] << 16) | ((size_t)w[pos + 7] << 24);
        if (memcmp(w + pos, "data", 4) == 0) { *len = (sz == 0 || sz == 0xFFFFFFFFu || pos + 8 + sz > n) ? n - pos - 8 : sz; return pos + 8; }
        pos += 8 + sz + (sz & 1);
    }
    printf("no data chunk\n"); exit(2);
}

/* Sample i of the source PCM (interleaved index), as the codec's signed integer. */
static int32_t wav_sample(const unsigned char *d, unsigned bits, size_t i) {
    if (bits == 8) return (int32_t)d[i] - 128;
    if (bits == 16) return (int16_t)(d[2 * i] | (d[2 * i + 1] << 8));
    if (bits == 32) return (int32_t)((uint32_t)d[4 * i] | ((uint32_t)d[4 * i + 1] << 8) | ((uint32_t)d[4 * i + 2] << 16) | ((uint32_t)d[4 * i + 3] << 24));
    int32_t v = d[3 * i] | (d[3 * i + 1] << 8) | (d[3 * i + 2] << 16);
    return (v << 8) >> 8;
}


/* A read callback over a memory buffer, counting the bytes it hands out. */
typedef struct { const unsigned char *data; size_t len; size_t served; int fail_after; int calls; } MemSrc;
static int mem_read(void *user, uint64_t pos, uint8_t *buf, size_t n) {
    MemSrc *m = user;
    if (m->fail_after >= 0 && m->calls++ >= m->fail_after) return 1;
    if (pos > m->len || n > m->len - (size_t)pos) return 1;
    memcpy(buf, m->data + pos, n);
    m->served += n;
    return 0;
}

/* A growable in-memory output behind the write callback. */
typedef struct { unsigned char *d; size_t len, cap; int calls, fail_after; } MemSink;
static int mem_write(void *user, uint64_t pos, const uint8_t *buf, size_t n) {
    MemSink *m = user;
    if (m->fail_after >= 0 && m->calls++ >= m->fail_after) return 1;
    if (pos + n > m->cap) { size_t c = (size_t)(pos + n) * 2; m->d = realloc(m->d, c); m->cap = c; }
    memcpy(m->d + pos, buf, n);
    if (pos + n > m->len) m->len = (size_t)(pos + n);
    return 0;
}
static int cancel_after_two(void *user, uint64_t done, uint64_t total) { (void)total; int *n = user; (*n)++; return done > 0 && *n > 2; }

static int cancel_after_first(void *user, uint64_t done, uint64_t total) { (void)user; (void)done; (void)total; return 1; }

int main(int argc, char **argv) {
    if (argc != 3) { printf("usage: capi_test <file.fak> <source.wav>\n"); return 2; }
    size_t fak_len, wav_len, pcm_len;
    unsigned char *fak = slurp(argv[1], &fak_len), *wav = slurp(argv[2], &wav_len);
    const unsigned char *pcm = wav + wav_data(wav, wav_len, &pcm_len);

    char err[256];
    FakDecoder *d = fak_decoder_open(fak, fak_len, err, sizeof err);
    if (!d) { printf("open failed: %s\n", err); return 1; }
    FakInfo info;
    CHECK(fak_decoder_info(d, &info) == 0, "info");
    printf("%u ch, %u bit, %u Hz, mode %u, %llu frames, %llu chunks, v%u\n", info.channels, info.bits_per_sample,
           info.sample_rate, info.mode, (unsigned long long)info.total_frames, (unsigned long long)info.chunk_count, info.format_version);
    const size_t nch = info.channels, bps = info.bits_per_sample / 8;
    CHECK(pcm_len == info.total_frames * nch * bps, "PCM length %zu vs %llu frames", pcm_len, (unsigned long long)info.total_frames);


    /* Callback-backed decoder: same info and same samples as the in-memory one, and it reads only what
     * it decodes (the metadata and index at open, then the chunks asked for). */
    {
        MemSrc ms = { fak, fak_len, 0, -1, 0 };
        FakDecoder *dc = fak_decoder_open_cb(mem_read, &ms, fak_len, err, sizeof err);
        CHECK(dc != NULL, "open_cb: %s", err);
        if (dc) {
            FakInfo ci;
            CHECK(fak_decoder_info(dc, &ci) == 0 && ci.total_frames == info.total_frames && ci.chunk_count == info.chunk_count, "open_cb info");
            CHECK(ms.served < fak_len / 2 + 1024 || info.chunk_count < 4, "open must not read the whole file (%zu of %zu)", ms.served, fak_len);
            size_t bad = 0;
            for (uint64_t c = 0; c < info.chunk_count; ++c) {
                uint64_t n = fak_decoder_chunk_frames(dc, c);
                int32_t *a = malloc(n * nch * sizeof(int32_t)), *b = malloc(n * nch * sizeof(int32_t));
                if (fak_decoder_decode_chunk(dc, c, a, n * nch) != (int64_t)n || fak_decoder_decode_chunk(d, c, b, n * nch) != (int64_t)n || memcmp(a, b, n * nch * sizeof(int32_t))) bad++;
                free(a); free(b);
            }
            CHECK(bad == 0, "%zu chunks differ between callback and in-memory decoders", bad);
            fak_decoder_free(dc);
        }
        /* A failing callback is an error, never a crash. */
        MemSrc fs = { fak, fak_len, 0, 3, 0 };
        FakDecoder *df = fak_decoder_open_cb(mem_read, &fs, fak_len, err, sizeof err);
        if (df) {
            for (uint64_t c = 0; c < info.chunk_count; ++c) {
                uint64_t n = fak_decoder_chunk_frames(df, c);
                int32_t *a = malloc(n * nch * sizeof(int32_t));
                if (fak_decoder_decode_chunk(df, c, a, n * nch) < 0) { free(a); break; }
                free(a);
            }
            fak_decoder_free(df);
        }
        /* Verification through the callback: passes on the file, fails on a damaged copy. */
        MemSrc vs = { fak, fak_len, 0, -1, 0 };
        CHECK(fak_verify_cb(mem_read, &vs, fak_len, 0, err, sizeof err) == 0, "verify_cb: %s", err);
        unsigned char *dmg = malloc(fak_len);
        memcpy(dmg, fak, fak_len);
        dmg[fak_len / 2] ^= 0x5a;
        MemSrc ds = { dmg, fak_len, 0, -1, 0 };
        CHECK(fak_verify_cb(mem_read, &ds, fak_len, 0, err, sizeof err) != 0, "verify_cb must reject a damaged file");
        free(dmg);
        MemSrc zs = { fak, fak_len, 0, -1, 0 };
        CHECK(fak_decoder_open_cb(mem_read, &zs, 10, err, sizeof err) == NULL, "truncated size must be rejected");
    }

    /* Full decode, chunk by chunk, against the source PCM. */
    uint64_t frame = 0;
    int32_t *buf = NULL;
    size_t mismatches = 0;
    for (uint64_t c = 0; c < info.chunk_count; ++c) {
        uint64_t n = fak_decoder_chunk_frames(d, c);
        CHECK(fak_decoder_chunk_start(d, c) == frame, "chunk %llu start", (unsigned long long)c);
        CHECK(fak_decoder_chunk_for_frame(d, frame) == (int64_t)c, "lookup at chunk start %llu", (unsigned long long)c);
        CHECK(fak_decoder_chunk_for_frame(d, frame + n - 1) == (int64_t)c, "lookup at chunk end %llu", (unsigned long long)c);
        buf = realloc(buf, n * nch * sizeof(int32_t));
        CHECK(fak_decoder_decode_chunk(d, c, buf, n * nch - 1) == -1, "undersized buffer must fail");
        int64_t got = fak_decoder_decode_chunk(d, c, buf, n * nch);
        CHECK(got == (int64_t)n, "chunk %llu decoded %lld frames", (unsigned long long)c, (long long)got);
        for (size_t i = 0; i < n * nch; ++i)
            if (buf[i] != wav_sample(pcm, info.bits_per_sample, frame * nch + i)) mismatches++;
        frame += n;
    }
    CHECK(frame == info.total_frames, "frames %llu", (unsigned long long)frame);
    CHECK(mismatches == 0, "%zu samples differ from the source", mismatches);
    CHECK(fak_decoder_chunk_for_frame(d, info.total_frames) == -1, "past-the-end lookup");
    CHECK(fak_decoder_decode_chunk(d, info.chunk_count, buf, 1) == -1, "out-of-range chunk must fail");
    printf("decode: %llu frames, %zu mismatches\n", (unsigned long long)frame, mismatches);

    /* Retag + pictures: the new file decodes to the same audio and carries the new metadata. */
    const char *tags[] = { "TITLE=C API test \xC3\xBC", "ARTIST=A", "ARTIST=B", "REPLAYGAIN_TRACK_GAIN=-6.50 dB" };
    static const unsigned char png[] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 1, 2, 3 };
    FakPictureIn pic = { 3, "image/png", "front", png, sizeof png };
    FakBuffer out = { 0 };
    CHECK(fak_rewrite_metadata(d, tags, 4, 1, &pic, 1, &out) == 0, "rewrite: %s", fak_decoder_last_error(d));
    FakDecoder *d2 = fak_decoder_open(out.data, out.len, err, sizeof err);
    CHECK(d2 != NULL, "reopen rewritten: %s", err);
    if (d2) {
        FakInfo i2;
        fak_decoder_info(d2, &i2);
        CHECK(memcmp(i2.pcm_sha256, info.pcm_sha256, 32) == 0, "PCM hash changed");
        CHECK(fak_decoder_tag_count(d2) == 4 && strcmp(fak_decoder_tag(d2, 0), tags[0]) == 0 && strcmp(fak_decoder_tag(d2, 2), "ARTIST=B") == 0, "tags");
        CHECK(fak_decoder_tag(d2, 4) == NULL, "tag past end");
        FakPicture p;
        CHECK(fak_decoder_picture_count(d2) == 1 && fak_decoder_picture(d2, 0, &p) == 0 && p.kind == 3 && p.data_len == sizeof png
              && memcmp(p.data, png, sizeof png) == 0 && strcmp(p.mime, "image/png") == 0, "picture");
        CHECK(fak_decoder_picture(d2, 1, &p) != 0, "picture past end");
        uint64_t c = info.chunk_count / 2, n = fak_decoder_chunk_frames(d2, c);
        int32_t *b2 = malloc(n * nch * sizeof(int32_t));
        CHECK(fak_decoder_decode_chunk(d2, c, b2, n * nch) == (int64_t)n, "decode after retag");
        for (size_t i = 0; i < n * nch; ++i)
            if (b2[i] != wav_sample(pcm, info.bits_per_sample, fak_decoder_chunk_start(d2, c) * nch + i)) { CHECK(0, "audio changed by retag"); break; }
        free(b2);
        /* Tags only (pictures kept), then back to the original tags: identical to the input file. */
        FakBuffer out2 = { 0 };
        CHECK(fak_rewrite_metadata(d2, NULL, 0, 0, NULL, 0, &out2) == 0, "clear tags");
        FakDecoder *d3 = fak_decoder_open(out2.data, out2.len, err, sizeof err);
        CHECK(d3 && fak_decoder_tag_count(d3) == 0 && fak_decoder_picture_count(d3) == 1, "tags cleared, picture kept");
        fak_decoder_free(d3);
        fak_buffer_free(out2);
        fak_decoder_free(d2);
    }
    fak_buffer_free(out);

    /* Cue sheet: a CUESHEET tag becomes the binary cue sheet (its text returned verbatim);
     * removing the tag removes it; a sheet pointing past the end of the audio is refused. */
    {
        const char *cue = "CUESHEET=FILE \"x.wav\" WAVE\r\n  TRACK 01 AUDIO\r\n    TITLE \"One\"\r\n    INDEX 01 00:00:00\r\n"
                          "  TRACK 02 AUDIO\r\n    INDEX 01 00:00:01\r\n";
        const char *ctags[] = { "TITLE=x", cue };
        CHECK(fak_decoder_cuesheet(d) == NULL, "no cue sheet in the source file");
        FakBuffer cb = { 0 };
        CHECK(fak_rewrite_metadata(d, ctags, 2, 0, NULL, 0, &cb) == 0, "add cue: %s", fak_decoder_last_error(d));
        FakDecoder *dc = fak_decoder_open(cb.data, cb.len, err, sizeof err);
        CHECK(dc && fak_decoder_cuesheet(dc) && strcmp(fak_decoder_cuesheet(dc), cue + 9) == 0, "cue text round trip");
        if (dc) {
            FakBuffer nb = { 0 };
            const char *plain[] = { "TITLE=x" };
            CHECK(fak_rewrite_metadata(dc, plain, 1, 0, NULL, 0, &nb) == 0, "remove cue");
            FakDecoder *dn = fak_decoder_open(nb.data, nb.len, err, sizeof err);
            CHECK(dn && fak_decoder_cuesheet(dn) == NULL, "cue removed with its tag");
            fak_decoder_free(dn);
            fak_buffer_free(nb);
            fak_decoder_free(dc);
        }
        fak_buffer_free(cb);
        char late[256];
        const unsigned secs = (unsigned)(info.total_frames / info.sample_rate);
        snprintf(late, sizeof late, "CUESHEET=FILE \"x.wav\" WAVE\r\n  TRACK 01 AUDIO\r\n    INDEX 01 %02u:%02u:00\r\n", secs / 60 + 1, secs % 60);
        const char *ltags[] = { late };
        FakBuffer lb = { 0 };
        CHECK(fak_rewrite_metadata(d, ltags, 1, 0, NULL, 0, &lb) == -1 && strstr(fak_decoder_last_error(d), "cue"), "cue past the end refused");
    }

    /* CD tags (opt-in) for a 16/44.1 stereo source of whole CD sectors: a cue sheet alone adds
     * none; in a file that has FAK_ tags (opted in), moving the sheet's track points recomputes them,
     * retitling leaves them, a CD tag the host changed wins, and removing the sheet drops the FAK_
     * ones but keeps DISCID. The host passes back every tag, as foobar2000 does. */
    if (info.channels == 2 && info.sample_rate == 44100 && info.bits_per_sample == 16 && info.total_frames % 588 == 0) {
        const unsigned half = (unsigned)(info.total_frames / 588 / 2), end = (unsigned)(info.total_frames / 588);
        char cueA[512], cueB[512], cueB2[512];
        #define CUE_FMT "CUESHEET=FILE \"x.wav\" WAVE\r\n  TRACK 01 AUDIO\r\n    TITLE \"%s\"\r\n    INDEX 01 00:00:00\r\n  TRACK 02 AUDIO\r\n    INDEX 01 %02u:%02u:%02u\r\n"
        snprintf(cueA, sizeof cueA, CUE_FMT, "One", half / 4500, half / 75 % 60, half % 75);
        snprintf(cueB, sizeof cueB, CUE_FMT, "One", (half + 1) / 4500, (half + 1) / 75 % 60, (half + 1) % 75);
        snprintf(cueB2, sizeof cueB2, CUE_FMT, "Renamed", (half + 1) / 4500, (half + 1) / 75 % 60, (half + 1) % 75);
        #define FIND(dec, key, out) do { out = NULL; for (size_t i_ = 0; i_ < fak_decoder_tag_count(dec); ++i_) \
            if (strncmp(fak_decoder_tag(dec, i_), key "=", strlen(key) + 1) == 0) out = fak_decoder_tag(dec, i_) + strlen(key) + 1; } while (0)
        /* Rewrites `src` with all its tags, the CUESHEET replaced by `cue` (NULL: removed), plus
         * `extra` (a "KEY=VALUE" replacing that key, or appended); `out` is the reopened file. */
        #define REWRITE(src, cue, extra, buf, out) do { size_t n_ = fak_decoder_tag_count(src), k_ = 0; int seen_ = 0; \
            const char **a_ = malloc((n_ + 3) * sizeof *a_); const char *x_ = (extra), *c_ = (cue); \
            for (size_t i_ = 0; i_ < n_; ++i_) { const char *t_ = fak_decoder_tag(src, i_); \
                if (strncmp(t_, "CUESHEET=", 9) == 0) continue; \
                if (x_ && strncmp(t_, x_, strcspn(x_, "=") + 1) == 0) { a_[k_++] = x_; seen_ = 1; continue; } a_[k_++] = t_; } \
            if (x_ && !seen_) a_[k_++] = x_; if (c_) a_[k_++] = c_; \
            CHECK(fak_rewrite_metadata(src, a_, k_, 0, NULL, 0, &buf) == 0, "cd rewrite: %s", fak_decoder_last_error(src)); \
            out = fak_decoder_open(buf.data, buf.len, err, sizeof err); free(a_); } while (0)
        const char *t1[] = { "TITLE=x", cueA };
        FakBuffer b1 = { 0 }, b2 = { 0 }, b3 = { 0 }, b4 = { 0 }, b5 = { 0 }, b6 = { 0 };
        CHECK(fak_rewrite_metadata(d, t1, 2, 0, NULL, 0, &b1) == 0, "cd: add cue: %s", fak_decoder_last_error(d));
        FakDecoder *e1 = fak_decoder_open(b1.data, b1.len, err, sizeof err), *e2 = NULL, *e3 = NULL, *e4 = NULL, *e5 = NULL, *e6 = NULL;
        const char *v = NULL, *v2 = NULL;
        if (e1) FIND(e1, "FAK_CD_TOC", v);
        CHECK(e1 && v == NULL, "cd: opt-in, a cue sheet alone adds none");
        if (e1) REWRITE(e1, cueA, "FAK_CD_TOC=mine", b2, e2);  /* the user adds a FAK_ tag: opted in */
        if (e2) FIND(e2, "FAK_CD_TOC", v);
        CHECK(v && strcmp(v, "mine") == 0, "cd: host-set value kept while no point moves");
        if (e2) REWRITE(e2, cueB, NULL, b3, e3);                /* track 2 moved */
        char want[64];
        snprintf(want, sizeof want, "0:%u:%u", half + 1, end);
        if (e3) { FIND(e3, "FAK_CD_TOC", v); FIND(e3, "FAK_ACCURATERIP_V2", v2); }
        CHECK(v && strcmp(v, want) == 0 && v2 && strlen(v2) == 17, "cd: move recomputes (toc %s)", v ? v : "none");
        char *v2_before = v2 ? _strdup(v2) : NULL;
        if (e3) REWRITE(e3, cueB2, NULL, b4, e4);               /* retitle only */
        if (e4) FIND(e4, "FAK_ACCURATERIP_V2", v2);
        CHECK(v2 && v2_before && strcmp(v2, v2_before) == 0, "cd: retitle keeps the tags");
        if (e4) REWRITE(e4, cueA, "DISCID=mine", b5, e5);       /* moved back, DISCID edited */
        snprintf(want, sizeof want, "0:%u:%u", half, end);
        if (e5) { FIND(e5, "FAK_CD_TOC", v); FIND(e5, "DISCID", v2); }
        CHECK(v && strcmp(v, want) == 0 && v2 && strcmp(v2, "mine") == 0, "cd: recomputed, the host's DISCID wins");
        if (e5) REWRITE(e5, NULL, NULL, b6, e6);                /* sheet removed */
        if (e6) { FIND(e6, "FAK_CD_TOC", v); FIND(e6, "DISCID", v2); }
        CHECK(e6 && fak_decoder_cuesheet(e6) == NULL && v == NULL && v2 && strcmp(v2, "mine") == 0, "cd: FAK_ tags dropped with the sheet, DISCID kept");
        printf("cd tags: opt-in, move, retitle, host edit, remove checked\n");
        free(v2_before);
        fak_decoder_free(e6); fak_decoder_free(e5); fak_decoder_free(e4); fak_decoder_free(e3); fak_decoder_free(e2); fak_decoder_free(e1);
        fak_buffer_free(b6); fak_buffer_free(b5); fak_buffer_free(b4); fak_buffer_free(b3); fak_buffer_free(b2); fak_buffer_free(b1);
    }

    const char *bad[] = { "\xff\xfe=invalid utf8" };
    CHECK(fak_rewrite_metadata(d, bad, 1, 0, NULL, 0, &out) == -1 && strlen(fak_decoder_last_error(d)) > 0, "invalid UTF-8 rejected");

    /* Hostile input: truncations and bit flips must fail cleanly or decode, never crash. */
    int rejected = 0, opened = 0;
    for (size_t cut = 0; cut < fak_len; cut += 1 + fak_len / 97) {
        FakDecoder *t = fak_decoder_open(fak, cut, err, sizeof err);
        if (t) { opened++; fak_decoder_free(t); } else rejected++;
    }
    CHECK(opened == 0, "%d truncated files opened", opened);
    unsigned char *mut = malloc(fak_len);
    int decoded_ok = 0, decode_errors = 0;
    for (int k = 0; k < 200; ++k) {
        memcpy(mut, fak, fak_len);
        mut[(size_t)k * 7919u * 104729u % fak_len] ^= (unsigned char)(1u << (k % 8));
        FakDecoder *t = fak_decoder_open(mut, fak_len, err, sizeof err);
        if (!t) continue;
        FakInfo ti;
        fak_decoder_info(t, &ti);
        uint64_t n = fak_decoder_chunk_frames(t, 0);
        int32_t *tb = malloc((n ? n : 1) * ti.channels * sizeof(int32_t));
        for (uint64_t c = 0; c < ti.chunk_count && c < 3; ++c) {
            uint64_t m = fak_decoder_chunk_frames(t, c);
            if (m > n) break;
            if (fak_decoder_decode_chunk(t, c, tb, n * ti.channels) < 0) decode_errors++; else decoded_ok++;
        }
        free(tb);
        fak_decoder_free(t);
    }
    printf("hostile: %d truncations rejected; bit flips: %d chunks decoded, %d rejected\n", rejected, decoded_ok, decode_errors);
    CHECK(fak_decoder_open(NULL, 0, err, sizeof err) == NULL, "empty input");
    fak_decoder_free(NULL);



    /* Streaming encode: same audio through fak_encoder_begin_stream must decode to the source and
     * verify, with only chunk-sized buffers held by the encoder; cancel and write failures are errors. */
    {
        const uint64_t frames = info.total_frames < 200000 ? info.total_frames : 200000;
        int32_t *pcm32 = malloc((size_t)frames * nch * sizeof(int32_t));
        for (size_t i = 0; i < (size_t)frames * nch; i++) pcm32[i] = wav_sample(pcm, info.bits_per_sample, i);
        const char *etags[] = { "TITLE=Streamed", "ARTIST=C API" };
        const int levels[] = { 0, 1, 3 };
        for (int li = 0; li < 3; ++li) {
            for (int fec = 0; fec < 2; ++fec) {
                MemSink sink = { NULL, 0, 0, 0, -1 };
                FakEncoder *e = fak_encoder_new((uint32_t)nch, info.bits_per_sample, info.sample_rate, frames);
                CHECK(fak_encoder_begin_stream(e, mem_write, &sink, levels[li], 2, 1, fec ? 3 : 0, etags, 2, NULL, 0, frames, NULL, NULL) == 0, "begin_stream: %s", fak_encoder_last_error(e));
                for (uint64_t at = 0; at < frames; ) {
                    uint64_t n2 = frames - at < 12345 ? frames - at : 12345;
                    CHECK(fak_encoder_push(e, pcm32 + at * nch, n2) == 0, "stream push: %s", fak_encoder_last_error(e));
                    at += n2;
                }
                CHECK(fak_encoder_end_stream(e) == 0, "end_stream: %s", fak_encoder_last_error(e));
                fak_encoder_free(e);
                CHECK(fak_verify(sink.d, sink.len, 0, err, sizeof err) == 0, "streamed file verifies: %s", err);
                FakDecoder *sd = fak_decoder_open(sink.d, sink.len, err, sizeof err);
                CHECK(sd != NULL, "streamed file opens: %s", err);
                if (sd) {
                    FakInfo si; fak_decoder_info(sd, &si);
                    CHECK(si.total_frames == frames && si.fec_group == (fec ? 3u : 0u), "streamed info: %llu frames, fec %u", (unsigned long long)si.total_frames, si.fec_group);
                    CHECK(fak_decoder_tag_count(sd) == 2, "streamed tags");
                    size_t sbad = 0; uint64_t at = 0;
                    for (uint64_t c = 0; c < si.chunk_count; ++c) {
                        uint64_t n2 = fak_decoder_chunk_frames(sd, c);
                        int32_t *b = malloc(n2 * nch * sizeof(int32_t));
                        if (fak_decoder_decode_chunk(sd, c, b, n2 * nch) != (int64_t)n2 || memcmp(b, pcm32 + at * nch, n2 * nch * sizeof(int32_t))) sbad++;
                        at += n2; free(b);
                    }
                    CHECK(sbad == 0, "streamed level %d fec %d: %zu chunks differ", levels[li], fec, sbad);
                    /* Retag through the streaming rewrite (the plugin's single-file conversion adds the cue
                     * sheet this way): same audio, new tags, a cue sheet, still verifies. */
                    {
                        const char *rtags[] = { "TITLE=Retagged", "ARTIST=C API", "CUESHEET=FILE \"a.wav\" WAVE\r\n  TRACK 01 AUDIO\r\n    INDEX 01 00:00:00\r\n" };
                        MemSink rsink = { NULL, 0, 0, 0, -1 };
                        CHECK(fak_rewrite_metadata_stream(sd, rtags, 3, 0, NULL, 0, mem_write, &rsink) == 0, "rewrite_stream: %s", fak_decoder_last_error(sd));
                        CHECK(fak_verify(rsink.d, rsink.len, 0, err, sizeof err) == 0, "rewritten file verifies: %s", err);
                        FakDecoder *rd = fak_decoder_open(rsink.d, rsink.len, err, sizeof err);
                        CHECK(rd != NULL && fak_decoder_tag_count(rd) == 3 && fak_decoder_cuesheet(rd) != NULL, "rewritten tags and cue sheet");
                        fak_decoder_free(rd); free(rsink.d);
                    }
                    fak_decoder_free(sd);
                }
                printf("stream level %d fec %d: %zu bytes\n", levels[li], fec, sink.len);
                free(sink.d);
            }
        }
        /* beginning after a push, on a float encoder, or twice is refused */
        { MemSink k = { NULL, 0, 0, 0, -1 }; FakEncoder *e = fak_encoder_new((uint32_t)nch, info.bits_per_sample, info.sample_rate, 0);
          fak_encoder_push(e, pcm32, 10);
          CHECK(fak_encoder_begin_stream(e, mem_write, &k, 1, 1, 0, 0, NULL, 0, NULL, 0, 0, NULL, NULL) != 0, "begin after push must fail");
          fak_encoder_free(e); free(k.d);
          FakEncoder *f = fak_encoder_new_float((uint32_t)nch, info.sample_rate, 0);
          CHECK(fak_encoder_begin_stream(f, mem_write, &k, 1, 1, 0, 0, NULL, 0, NULL, 0, 0, NULL, NULL) != 0, "float encoder cannot stream");
          fak_encoder_free(f); }
        /* cancel through the progress callback */
        { MemSink k = { NULL, 0, 0, 0, -1 }; int n = 0; FakEncoder *e = fak_encoder_new((uint32_t)nch, info.bits_per_sample, info.sample_rate, frames);
          CHECK(fak_encoder_begin_stream(e, mem_write, &k, 0, 1, 1, 0, NULL, 0, NULL, 0, frames, (FakProgress)cancel_after_two, &n) == 0, "begin for cancel");
          int rc = 0;
          for (uint64_t at = 0; at < frames && rc == 0; ) { uint64_t n2 = frames - at < 12345 ? frames - at : 12345; rc = fak_encoder_push(e, pcm32 + at * nch, n2); at += n2; }
          CHECK(rc == -1 && strcmp(fak_encoder_last_error(e), "cancelled") == 0 || frames < 3 * info.sample_rate, "cancel reported: %s", fak_encoder_last_error(e));
          fak_encoder_free(e); free(k.d); }
        /* a failing sink is an error, not a crash */
        { MemSink k = { NULL, 0, 0, 0, 3 }; FakEncoder *e = fak_encoder_new((uint32_t)nch, info.bits_per_sample, info.sample_rate, frames);
          int rc = fak_encoder_begin_stream(e, mem_write, &k, 0, 1, 1, 0, NULL, 0, NULL, 0, frames, NULL, NULL);
          for (uint64_t at = 0; at < frames && rc == 0; ) { uint64_t n2 = frames - at < 12345 ? frames - at : 12345; rc = fak_encoder_push(e, pcm32 + at * nch, n2); at += n2; }
          if (rc == 0) rc = fak_encoder_end_stream(e);
          CHECK(rc != 0, "a failing sink must fail the encode");
          fak_encoder_free(e); free(k.d); }
        free(pcm32);
    }

    /* Encoding through the C API: every level must reproduce the source PCM exactly (checked twice:
     * fak_verify against the header hash, and a chunk-by-chunk decode against the source samples). */
    {
        const uint64_t frames = info.total_frames < 150000 ? info.total_frames : 150000;
        int32_t *pcm32 = malloc((size_t)frames * nch * sizeof(int32_t));
        for (size_t i = 0; i < (size_t)frames * nch; i++) pcm32[i] = wav_sample(pcm, info.bits_per_sample, i);
        static const char *names[] = { "fast", "normal", "max", "insane", "archival" };
        const char *etags[] = { "TITLE=Encoded", "ARTIST=C API" };
        static const unsigned char png[] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 9, 8, 7 };
        const FakPictureIn epic = { 3, "image/png", "", png, sizeof png };
        for (int level = 0; level < 5; level++) {
            FakEncoder *e = fak_encoder_new((uint32_t)nch, info.bits_per_sample, info.sample_rate, frames);
            CHECK(e != NULL, "encoder_new");
            if (!e) continue;
            /* Push in odd-sized blocks, like a decoder handing over whatever chunk size it has. */
            for (uint64_t at = 0; at < frames;) {
                uint64_t n = 4099 < frames - at ? 4099 : frames - at;
                CHECK(fak_encoder_push(e, pcm32 + at * nch, (size_t)n) == 0, "push");
                at += n;
            }
            FakBuffer eo = { 0 };
            int rc = fak_encoder_finish(e, level < 4 ? level : 3, 0, 0, level == 4 ? 16 : 0, 0, etags, 2, &epic, 1, NULL, NULL, &eo);
            CHECK(rc == 0, "finish level %d: %s", level, fak_encoder_last_error(e));
            if (rc == 0) {
                CHECK(fak_verify(eo.data, eo.len, 0, err, sizeof err) == 0, "verify level %d: %s", level, err);
                FakDecoder *ed = fak_decoder_open(eo.data, eo.len, err, sizeof err);
                CHECK(ed != NULL, "open encoded level %d", level);
                if (ed) {
                    FakInfo ei;
                    fak_decoder_info(ed, &ei);
                    CHECK(ei.total_frames == frames && ei.channels == nch && ei.bits_per_sample == info.bits_per_sample && ei.sample_rate == info.sample_rate, "encoded stream parameters");
                    CHECK(fak_decoder_tag_count(ed) == 2 && strcmp(fak_decoder_tag(ed, 1), "ARTIST=C API") == 0, "encoded tags");
                    CHECK(fak_decoder_picture_count(ed) == 1, "encoded picture");
                    int32_t *cb = malloc((size_t)fak_decoder_chunk_frames(ed, 0) * nch * sizeof(int32_t) + 4);
                    uint64_t at = 0;
                    for (uint64_t c = 0; c < ei.chunk_count; c++) {
                        uint64_t n = fak_decoder_chunk_frames(ed, c);
                        CHECK(fak_decoder_decode_chunk(ed, c, cb, (size_t)n * nch) == (int64_t)n, "decode encoded chunk");
                        for (size_t i = 0; i < (size_t)n * nch; i++)
                            if (cb[i] != pcm32[at * nch + i]) { CHECK(0, "level %d: sample mismatch at frame %llu", level, (unsigned long long)at); break; }
                        at += n;
                    }
                    free(cb);
                    fak_decoder_free(ed);
                }
                printf("level %-8s %zu bytes for %llu frames\n", names[level], eo.len, (unsigned long long)frames);
                /* A flipped bit in the payload must fail verification. */
                if (eo.len > 200) {
                    eo.data[eo.len - 100] ^= 0x10;
                    CHECK(fak_verify(eo.data, eo.len, 0, err, sizeof err) != 0 || level == 4, "verify must reject a damaged stream (level %d)", level);
                }
            }
            fak_buffer_free(eo);
            fak_encoder_free(e);
        }

        /* FEC at a lower level, and an explicit chunk length: still exact, and FEC repairs one damaged chunk. */
        {
            const uint64_t fr = frames;
            FakEncoder *e = fak_encoder_new((uint32_t)nch, info.bits_per_sample, info.sample_rate, fr);
            fak_encoder_push(e, pcm32, (size_t)fr);
            FakBuffer plain = { 0 }, fec = { 0 };
            CHECK(fak_encoder_finish(e, 0, 2, 1, 0, 0, NULL, 0, NULL, 0, NULL, NULL, &plain) == 0, "fast, 1 s chunks, no FEC: %s", fak_encoder_last_error(e));
            fak_encoder_free(e);
            e = fak_encoder_new((uint32_t)nch, info.bits_per_sample, info.sample_rate, fr);
            fak_encoder_push(e, pcm32, (size_t)fr);
            CHECK(fak_encoder_finish(e, 0, 2, 1, 4, 0, NULL, 0, NULL, 0, NULL, NULL, &fec) == 0, "fast, 1 s chunks, FEC 1/4: %s", fak_encoder_last_error(e));
            CHECK(fak_encoder_finish(e, 0, 2, 1, 0xFFFFFFFFu, 0, NULL, 0, NULL, 0, NULL, NULL, &fec) == 0, "fast, whole-file Reed-Solomon FEC: %s", fak_encoder_last_error(e));
            CHECK(fak_encoder_finish(e, 0, 0, 0, 60001, 0, NULL, 0, NULL, 0, NULL, NULL, &fec) == -1, "FEC group 60001 must be refused");
            fak_encoder_free(e);
            if (plain.data && fec.data) {
                CHECK(fak_verify(plain.data, plain.len, 0, err, sizeof err) == 0 && fak_verify(fec.data, fec.len, 0, err, sizeof err) == 0, "verify with/without FEC");
                CHECK(fec.len > plain.len, "FEC adds parity data (%zu vs %zu bytes)", fec.len, plain.len);
                FakDecoder *pd = fak_decoder_open(plain.data, plain.len, err, sizeof err);
                FakInfo pi;
                if (pd && fak_decoder_info(pd, &pi) == 0) CHECK(pi.chunk_count == (fr + info.sample_rate - 1) / info.sample_rate, "1 s chunks: %llu chunks for %llu frames", (unsigned long long)pi.chunk_count, (unsigned long long)fr);
                fak_decoder_free(pd);
                printf("FEC 1/4 at fast: %zu bytes vs %zu without\n", fec.len, plain.len);
                /* damage the middle of the FEC file's payload: it must still verify (repaired) when there are >= 2 chunks */
                if (fec.len > 4000) {
                    fec.data[fec.len / 3] ^= 0x55;
                    int repaired = fak_verify(fec.data, fec.len, 0, err, sizeof err) == 0;
                    printf("FEC file with a damaged byte: %s\n", repaired ? "still verifies (repaired)" : "does not verify");
                }
            }
            fak_buffer_free(plain); fak_buffer_free(fec);
        }


        /* Encoder string and stream profile (chunk length, FEC), and CD tags: written for an exact CD image with a
         * cue sheet, absent when not asked for and when the layout is not exact. */
        {
            enum { SECTORS = 375, FR = SECTORS * 588 };
            int32_t *cd = malloc((size_t)FR * 2 * sizeof(int32_t));
            for (size_t i = 0; i < (size_t)FR * 2; i++) cd[i] = (int32_t)((i * 7919u) % 20000u) - 10000;
            const char *cue = "FILE \"CDImage.wav\" WAVE\r\n  TRACK 01 AUDIO\r\n    INDEX 01 00:00:00\r\n  TRACK 02 AUDIO\r\n    INDEX 01 00:01:25\r\n  TRACK 03 AUDIO\r\n    INDEX 01 00:02:50\r\n";
            char cuetag[512];
            snprintf(cuetag, sizeof cuetag, "CUESHEET=%s", cue);
            const char *ctags[] = { cuetag };
            static const char *modes[] = { "CD tags on, exact image", "CD tags off", "CD tags on, length not a whole number of sectors" };
            for (int mode = 0; mode < 3; mode++) {
                const uint64_t fr = mode == 2 ? FR - 1 : FR;
                FakEncoder *e = fak_encoder_new(2, 16, 44100, fr);
                fak_encoder_push(e, cd, (size_t)fr);
                FakBuffer co = { 0 };
                int crc = fak_encoder_finish(e, 3, 0, 2, 5, mode != 1, ctags, 1, NULL, 0, NULL, NULL, &co);
                CHECK(crc == 0, "%s: %s", modes[mode], fak_encoder_last_error(e));
                if (crc == 0) {
                    FakDecoder *cdd = fak_decoder_open(co.data, co.len, err, sizeof err);
                    CHECK(cdd != NULL, "%s: open", modes[mode]);
                    if (cdd) {
                        const char *vendor = fak_decoder_vendor(cdd);
                        CHECK(strstr(vendor, "foo_input_fak") && strstr(vendor, "level=insane") && strstr(vendor, "fec=RS per 5") && strstr(vendor, "chunk=2s"), "encoder string: %s", vendor);
                        FakInfo ci;
                        CHECK(fak_decoder_info(cdd, &ci) == 0 && ci.chunk_frames == 88200 && ci.fec_group == 3 && ci.parity_blocks == 1 /* 3 chunks: one group, smaller than the 5 asked for */, "profile: chunk_frames %u fec_group %u parity %u", ci.chunk_frames, ci.fec_group, ci.parity_blocks);
                        CHECK(fak_decoder_cuesheet(cdd) != NULL, "%s: cue sheet stored", modes[mode]);
                        int has = 0, has_ctdb = 0;
                        for (size_t i = 0, n = fak_decoder_tag_count(cdd); i < n; i++) {
                            const char *t = fak_decoder_tag(cdd, i);
                            if (_strnicmp(t, "DISCID=", 7) == 0) has = 1;
                            if (_strnicmp(t, "FAK_CTDB_CRC=", 13) == 0) has_ctdb = 1;
                        }
                        CHECK(mode == 0 ? (has && has_ctdb) : (!has && !has_ctdb), "%s: CD tags present=%d ctdb=%d", modes[mode], has, has_ctdb);
                        printf("CD tags, %s: %s; vendor \"%s\"\n", modes[mode], has ? "written" : "not written", vendor);
                        fak_decoder_free(cdd);
                    }
                    CHECK(fak_verify(co.data, co.len, 0, err, sizeof err) == 0, "%s: verify", modes[mode]);
                }
                fak_buffer_free(co);
                fak_encoder_free(e);
            }
            free(cd);
        }

        /* 32-bit float through the C API: stored losslessly, every bit pattern back exactly. */
        {
            enum { FR = 100000, CH = 2 };
            float *fin = malloc(sizeof(float) * FR * CH), *fout = malloc(sizeof(float) * FR * CH);
            uint32_t seed = 12345;
            for (int variant = 0; variant < 3; variant++) {
                for (int i = 0; i < FR * CH; i++) {
                    seed = seed * 1664525u + 1013904223u;
                    if (variant == 0) fin[i] = (float)((int)(seed >> 8) % 8388608) / 8388608.0f;              /* 24-bit grid in float */
                    else fin[i] = ((float)(seed >> 8) / 16777216.0f - 0.5f) * (float)(1 + (i % 7));            /* arbitrary floats */
                }
                if (variant == 2) {
                    static const uint32_t odd[] = { 0x7fc00000u, 0xff800000u, 0x7f800000u, 0x80000000u, 0x00000001u, 0x007fffffu, 0x7f7fffffu, 0xff7fffffu };
                    for (int i = 0; i < 4000; i++) memcpy(&fin[i * 37 % (FR * CH)], &odd[i % 8], 4);
                }
                FakEncoder *e = fak_encoder_new_float(CH, 48000, FR);
                CHECK(e != NULL, "float encoder");
                CHECK(fak_encoder_push_float(e, fin, 33333) == 0 && fak_encoder_push_float(e, fin + 33333 * CH, FR - 33333) == 0, "push_float");
                CHECK(fak_encoder_push(e, (const int32_t *)fin, 1) == -1, "integer push on a float encoder must fail");
                FakBuffer fo = { 0 };
                int frc = fak_encoder_finish(e, 1, 0, 0, 0, 0, NULL, 0, NULL, 0, NULL, NULL, &fo);
                CHECK(frc == 0, "float finish: %s", fak_encoder_last_error(e));
                if (frc == 0) {
                    CHECK(fak_verify(fo.data, fo.len, 0, err, sizeof err) == 0, "float verify: %s", err);
                    FakDecoder *fd = fak_decoder_open(fo.data, fo.len, err, sizeof err);
                    FakInfo fi;
                    CHECK(fd && fak_decoder_info(fd, &fi) == 0 && fi.is_float == 1 && fi.total_frames == FR && fi.channels == CH && fi.sample_rate == 48000, "float stream info");
                    if (fd) {
                        CHECK(fak_decoder_decode_chunk_float(fd, 0, fout, 1) == -1, "undersized float buffer must fail");
                        uint64_t at = 0; int bad = 0;
                        for (uint64_t c = 0; c < fi.chunk_count; c++) {
                            uint64_t n = fak_decoder_chunk_frames(fd, c);
                            CHECK(fak_decoder_decode_chunk_float(fd, c, fout, (size_t)n * CH) == (int64_t)n, "float chunk decode");
                            if (memcmp(fout, fin + at * CH, (size_t)n * CH * 4) != 0) bad++;
                            at += n;
                        }
                        CHECK(bad == 0 && at == FR, "float variant %d: %d chunks differ", variant, bad);
                        fak_decoder_free(fd);
                    }
                    printf("float variant %d: %zu bytes for %d frames (%d bytes of float32)\n", variant, fo.len, FR, FR * CH * 4);
                }
                fak_buffer_free(fo);
                fak_encoder_free(e);
            }
            /* An integer stream refuses the float decode call. */
            CHECK(fak_decoder_decode_chunk_float(d, 0, fout, (size_t)FR * CH) == -1, "integer stream must refuse float decode");
            free(fin); free(fout);
        }

        /* Out-of-range sample values are refused, and nothing is stored. */
        {
            FakEncoder *e = fak_encoder_new(1, 16, 44100, 0);
            int32_t bad[2] = { 0, 40000 };
            CHECK(e && fak_encoder_push(e, bad, 2) == -1, "out-of-range sample must be refused");
            CHECK(fak_encoder_new(0, 16, 44100, 0) == NULL && fak_encoder_new(2, 12, 44100, 0) == NULL && fak_encoder_new(2, 16, 0, 0) == NULL && fak_encoder_new(2, 33, 44100, 0) == NULL, "bad parameters");
            { FakEncoder *e32 = fak_encoder_new(2, 32, 44100, 0); CHECK(e32 != NULL, "32-bit integer encoder"); fak_encoder_free(e32); }
            fak_encoder_free(e);
        }
        /* Cancelling from the progress callback ends the encode with an error and no output. */
        {
            FakEncoder *e = fak_encoder_new((uint32_t)nch, info.bits_per_sample, info.sample_rate, frames);
            fak_encoder_push(e, pcm32, (size_t)frames);
            FakBuffer eo = { 0 };
            CHECK(fak_encoder_finish(e, 0, 0, 0, 0, 0, NULL, 0, NULL, 0, cancel_after_first, NULL, &eo) == -1 && eo.data == NULL, "cancel");
            CHECK(strcmp(fak_encoder_last_error(e), "cancelled") == 0, "cancel message: %s", fak_encoder_last_error(e));
            fak_encoder_free(e);
        }
        free(pcm32);
    }

    fak_decoder_free(d);
    free(buf); free(mut); free(fak); free(wav);
    printf(failures ? "FAILED (%d)\n" : "OK\n", failures);
    return failures ? 1 : 0;
}
