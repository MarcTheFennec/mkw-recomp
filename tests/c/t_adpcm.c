// DSP-ADPCM voices (runtime/audio/audio_mixer.c, the section "voice decoding along the voice clock's sample path",
// which test_adpcm.py cuts out into voice.inc with a decode counter) against an exact decode: each buffer decoded once,
// from its start with its context or, without a context, from the frame boundary with the history the buffer played
// before it ended on. That second rule is our model of the DSP, not a recording of the console.
// Random buffers (predictor 0-7, scale 2^0..2^8), five sample rates, seven pitches up to 3.9 (the high case reaches
// 192 kHz and pitch 64), 400 voices of 300 render frames of 240 samples. The voice advances as advance_voice in
// audren.c does (without its counters).
// Usage: t_adpcm ctx|mixed|high   ctx: every buffer has a context, loops too; mixed: half have none, no loops;
// high: mixed contexts, with pitches up to 64 and sample rates up to 192 kHz to cross more than 128 samples at once.
// Prints: output samples, samples that differ from the exact decode, decodes.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audren.h"

#define MAX_FRAME 1024
static unsigned long long decodes;
#include "voice.inc"

int audio_buffer_empty(const AudrenWaveBuffer *b) { return !b->address || !b->size || b->end <= b->start; }

static uint32_t seed = 1;
static int rnd(void) { seed = seed * 1103515245u + 12345u; return (int)((seed >> 16) & 0x7fff); }

static void advance(ServerVoice *v, double out_seconds) {
    double want = v->fraction + out_seconds * v->in.sample_rate * audio_pitch(v->in.pitch);
    uint64_t samples = (uint64_t)want;
    v->fraction = want - (double)samples;
    while (samples && v->valid[v->index]) {
        AudrenWaveBuffer *b = &v->buffers[v->index];
        if (b->end <= b->start && b->loop && b->loop_count < 0) break;
        uint64_t length = (uint64_t)(b->end - b->start), left = length - v->offset, step = samples < left ? samples : left;
        v->offset += step, samples -= step;
        if (v->offset < length) break;
        if (b->loop && (b->loop_count < 0 || v->loops_done < b->loop_count)) {
            v->loops_done++;
            v->offset = b->loop_start > b->start && b->loop_start < b->end ? (uint64_t)(b->loop_start - b->start) : 0;
            continue;
        }
        v->valid[v->index] = 0, v->offset = 0, v->loops_done = 0, v->index = (v->index + 1) & 3;
        if (b->end_of_stream) break;
    }
    while (v->valid[v->index] && audio_buffer_empty(&v->buffers[v->index]))
        v->valid[v->index] = 0, v->offset = 0, v->loops_done = 0, v->index = (v->index + 1) & 3;
}

static uint8_t data[8][4096];
static int16_t ctx[8][3], coef[16] = {2048, 0, 3840, -1792, 1920, 0, 3584, -1536, 1024, 512, 2900, -1000, 0, 0, 3000, -1200};
static int16_t *exact[4];   // the exact decode of the buffer in each slot, by sample index
static int16_t last_h1, last_h2;   // the history the buffer played last ended on

static void exact_decode(int slot, const AudrenWaveBuffer *b) {
    const uint8_t *d = (const uint8_t *)(uintptr_t)b->address;
    int16_t h1 = last_h1, h2 = last_h2;
    if (b->context_address) h1 = ((const int16_t *)(uintptr_t)b->context_address)[1], h2 = ((const int16_t *)(uintptr_t)b->context_address)[2];
    free(exact[slot]);
    exact[slot] = calloc((size_t)b->end + 1, 2);
    for (uint64_t i = b->context_address ? (uint64_t)b->start : (uint64_t)(b->start - b->start % 14); i < (uint64_t)b->end; i++) {
        uint64_t frame = i / 14, nib = i % 14;
        int16_t s = 0;
        if ((frame + 1) * 8 <= b->size) {
            int scale = 1 << (d[frame * 8] & 15), pred = (d[frame * 8] >> 4) & 7;
            int n = (d[frame * 8 + 1 + nib / 2] >> ((nib & 1) ? 0 : 4)) & 15;
            if (n >= 8) n -= 16;
            int64_t x = ((int64_t)n * scale * 2048 + 1024 + (int64_t)coef[pred * 2] * h1 + (int64_t)coef[pred * 2 + 1] * h2) >> 11;
            s = (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
        }
        h2 = h1, h1 = s, exact[slot][i] = s;
    }
    last_h1 = h1, last_h2 = h2;
}

static float exact_at(const Walk *w) {
    if (w->ended || !w->valid[w->index]) return 0;
    const AudrenWaveBuffer *b = &w->v->buffers[w->index];
    uint64_t i = (uint64_t)b->start + w->offset;
    return i < (uint64_t)b->end ? exact[w->index][i] / 32768.0f : 0;
}

// voice_render's walk and interpolation, with the samples taken from the exact decode
static void render_exact(AudioRenderer *r, ServerVoice *v, float *out, int frames) {
    const double step = v->in.sample_rate * audio_pitch(v->in.pitch) / r->param.sample_rate;
    Walk w = {.v = v, .index = v->index, .offset = v->offset, .loops = v->loops_done, .abuf = -1};
    memcpy(w.valid, v->valid, sizeof w.valid);
    walk_skip_empty(&w);
    double frac = v->fraction;
    for (int k = 0; k < frames; k++) {
        Walk next = w;
        walk_step(&next);
        float s0 = exact_at(&w), s1 = exact_at(&next);
        out[k] = (float)(s0 + (s1 - s0) * frac);
        frac += step;
        while (frac >= 1.0) frac -= 1.0, walk_step(&w);
    }
}

static void fill(ServerVoice *v, int slot, int with_ctx, int loops) {
    AudrenWaveBuffer *b = &v->buffers[slot];
    int which = rnd() % 8, frames = 50 + rnd() % 250;
    uint8_t *copy = malloc((size_t)frames * 8);   // a new address while the old one is still held
    memcpy(copy, data[which], (size_t)frames * 8);
    free((void *)(uintptr_t)b->address);
    memset(b, 0, sizeof *b);
    b->address = (uint64_t)(uintptr_t)copy, b->size = (uint64_t)frames * 8;
    b->start = rnd() % 3 == 0 ? rnd() % 40 : 0;
    b->end = frames * 14 - (rnd() % 2 ? rnd() % 13 : 0);
    if (with_ctx || rnd() % 2) b->context_address = (uint64_t)(uintptr_t)ctx[which], b->context_size = 6;
    if (loops && rnd() % 2) {
        b->loop = 1, b->loop_count = rnd() % 3 == 0 ? -1 : rnd() % 3, b->loop_end = b->end;
        b->loop_start = rnd() % 2 ? b->start + rnd() % ((b->end - b->start) / 2) : 0;
    }
    v->valid[slot] = 1;
    exact_decode(slot, b);
}

int main(int argc, char **argv) {
    int all_ctx = argc > 1 && !strcmp(argv[1], "ctx");
    int high = argc > 1 && !strcmp(argv[1], "high");
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 4096; j++) data[i][j] = (uint8_t)(j % 8 ? rnd() : ((rnd() % 8) << 4) | (rnd() % 9));
        for (int k = 0; k < 3; k++) ctx[i][k] = (int16_t)(rnd() * 2 - 32768);
    }
    static const double rates[] = {48000, 32000, 22050, 44100, 16000};
    static const float pitches[] = {1, 0.5f, 0.73f, 1.37f, 2.3f, 0.99f, 3.9f};
    static const double high_rates[] = {192000, 32000, 22050, 44100, 16000};
    static const float high_pitches[] = {64, 0.5f, 0.73f, 1.37f, 2.3f, 0.99f, 3.9f};
    unsigned long long outs = 0, wrong = 0;
    static float got[6][MAX_FRAME], want[MAX_FRAME];
    for (int voice = 0; voice < 400; voice++) {
        static ServerVoice v;
        for (int s = 0; s < 4; s++) free((void *)(uintptr_t)v.buffers[s].address);
        memset(&v, 0, sizeof v);
        v.in.sample_format = FMT_ADPCM, v.in.channel_count = 1, v.in.extra_address = (uint64_t)(uintptr_t)coef, v.in.extra_size = 32;
        v.in.sample_rate = (uint32_t)(high ? high_rates[voice % 5] : rates[voice % 5]);
        v.in.pitch = high ? high_pitches[voice % 7] : pitches[voice % 7];
        last_h1 = last_h2 = 0;
        for (int s = 0; s < 4; s++) fill(&v, s, all_ctx, all_ctx);
        v.state = SV_STARTED, v.in.in_use = 1;
        AudioRenderer r;
        memset(&r, 0, sizeof r);
        r.param.sample_rate = 48000;
        for (int f = 0; f < 300; f++) {
            render_exact(&r, &v, want, 240);
            voice_render(&r, &v, got, 240);
            for (int k = 0; k < 240; k++) wrong += got[0][k] != want[k];
            outs += 240;
            advance(&v, 240 / 48000.0);
            for (int j = 0; j < 4; j++)   // refill in play order, so each buffer follows the one played before it
                if (!v.valid[(v.index + j) & 3]) fill(&v, (v.index + j) & 3, all_ctx, all_ctx);
        }
    }
    printf("%llu %llu %llu\n", outs, wrong, decodes);
    return 0;
}
