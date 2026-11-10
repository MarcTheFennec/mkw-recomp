// MKW_REUSE: the recorder ring's side of reusing a draw's packets in a
// later frame. vk_exec_reuse.c decides; here:
// - the bound state of the part being recorded, one hash per slot (pipeline, each vertex buffer, index buffer, each
//   dynamic state, blend state per target, the pass), updated by every packet the producer publishes. A draw's cached
//   packets are valid only where the state they did not set themselves equals what it was when they were recorded.
//   Descriptor sets are not slots: a reused draw binds its sets again (vk_exec_reuse.c).
// - the capture of the packets one draw publishes, with the pointers into each packet made relative to it, and the
//   bound-state slots before it and the ones it wrote;
// - their emission into the ring again, with the pointers moved to where they land.
#define VK_REC_IMPL
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "vk_rec_int.h"
#include "vk_rec_reuse.h"

enum {
    SL_PIPE = 0,                 // +0 graphics, +1 compute
    SL_VB = 2,                   // +binding (17: the replay's 16 and the stand-in, vk_exec_cb.c)
    SL_IB = SL_VB + 17,
    SL_OP = SL_IB + 1,           // +packet op: the single-valued dynamic states (R_ ops below 64)
    SL_BOOL = SL_OP + 64,        // +0/+1: the two commands rec_bool serves (depth clamp, alpha to coverage)
    SL_BLEND_EN = SL_BOOL + 2,   // +target (8)
    SL_BLEND_EQ = SL_BLEND_EN + 8,
    SL_WMASK = SL_BLEND_EQ + 8,
    SL_RENDER = SL_WMASK + 8,    // the open pass (BeginRendering), 0 none
    SL_PUSHC = SL_RENDER + 1,
    SL_N_ = SL_PUSHC + 1,
};
_Static_assert(SL_N_ <= REC_SLOTS, "REC_SLOTS covers the slots");
_Static_assert(R_Checkpoint < 64, "packet ops fit SL_OP");

int rec_slots_on;
static uint64_t slot[REC_SLOTS];
static void *bool_fn[2];
static int emitting;             // rec_win_emit: slots are set from the window's pairs, not hashed again
static struct {
    int on, bad, why;            // why: the last capture's refusal (rec_cap_why)
    uint32_t pk, np;             // packets published since the capture began; pairs noted (ordinal, slot, value)
    uint16_t pord[REC_WIN_PAIRS];
    uint8_t psl[REC_WIN_PAIRS];
    uint64_t ph[REC_WIN_PAIRS];
    uint64_t pos;                // put_h when the capture began
    RecWin *w;
} CAP;

static uint64_t mix(uint64_t h, uint64_t w) { return (h ^ w) * 0xBF58476D1CE4E5B9ull ^ ((h ^ w) >> 29); }

// Hash of n bytes at v (a packet's array or fixed part), 8 bytes at a time with the tail zero-padded.
static uint64_t hbytes(uint64_t h, const void *v, size_t n) {
    const uint8_t *p = v;
    for (size_t i = 0; i < n; i += 8) {
        uint64_t w = 0;
        memcpy(&w, p + i, n - i < 8 ? n - i : 8);
        h = mix(h, w);
    }
    return h;
}

// A packet's words with its pointers into itself made relative, so equal packets hash equal wherever they lie.
static uint64_t hpacket(uint32_t op, const uint64_t *p, uint32_t words) {
    uint64_t h = op + 1, lo = (uint64_t)(uintptr_t)p, hi = lo + 8ull * words;
    for (uint32_t i = 0; i < words; i++) h = mix(h, p[i] >= lo && p[i] < hi ? p[i] - lo : p[i]);
    return h | 1;   // 0 is an empty slot
}

static void set_slot(int s, uint64_t h) {
    slot[s] = h;
    if (!CAP.on) return;
    CAP.w->written[s >> 6] |= 1ull << (s & 63);
    if (CAP.np >= REC_WIN_PAIRS) { CAP.bad = 5; return; }
    CAP.pord[CAP.np] = (uint16_t)CAP.pk, CAP.psl[CAP.np] = (uint8_t)s, CAP.ph[CAP.np] = h, CAP.np++;
}

void rec_slots_reset(void) {
    memset(slot, 0, sizeof slot);   // a new command buffer starts with nothing bound
    if (CAP.on) CAP.bad = 1;        // (CAP.bad: 1 a new part, 2 a fallback, 3 more than 16 vertex buffers, 4 8 targets)
}

static void rec_slot_op(uint32_t op, const uint64_t *p, uint32_t words);
void rec_slot_note(uint32_t op, const uint64_t *p, uint32_t words) {
    if (emitting) return;
    rec_slot_op(op, p, words);
    CAP.pk++;
}

static void rec_slot_op(uint32_t op, const uint64_t *p, uint32_t words) {
    switch (op) {
    case R_BindPipeline: {
        const A_BindPipeline *a = (const void *)p;
        set_slot(SL_PIPE + (a->bp == VK_PIPELINE_BIND_POINT_COMPUTE), hpacket(op, p, words));
        break;
    }
    case R_BindVertexBuffers2: {
        const A_BindVertexBuffers2 *a = (const void *)p;
        for (uint32_t i = 0; i < a->n && a->first + i < 17; i++) {
            uint64_t h = mix(mix(op, (uint64_t)(uintptr_t)a->b[i]), a->o[i]);
            h = mix(mix(h, a->sz ? a->sz[i] : ~0ull), a->st ? a->st[i] : ~0ull);
            set_slot(SL_VB + (int)(a->first + i), h | 1);
        }
        if (a->first + a->n > 17 && CAP.on) CAP.bad = 3;
        break;
    }
    case R_BindIndexBuffer: set_slot(SL_IB, hpacket(op, p, words)); break;
    case R_Bool: {
        const A_Bool *a = (const void *)p;
        int k = bool_fn[0] == (void *)a->fn ? 0 : bool_fn[1] == (void *)a->fn ? 1 : -1;
        if (k < 0 && !bool_fn[0]) bool_fn[0] = (void *)a->fn, k = 0;
        else if (k < 0 && !bool_fn[1]) bool_fn[1] = (void *)a->fn, k = 1;
        if (k < 0) rt_fatal("vk_rec_reuse: a third command through rec_bool");
        set_slot(SL_BOOL + k, hpacket(op, p, words));
        break;
    }
    case R_BlendEnable: case R_BlendEquation: case R_WriteMask: {
        const A_Targets *a = (const void *)p;
        size_t el = op == R_BlendEquation ? sizeof(VkColorBlendEquationEXT) : 4;
        int base = op == R_BlendEnable ? SL_BLEND_EN : op == R_BlendEquation ? SL_BLEND_EQ : SL_WMASK;
        for (uint32_t i = 0; i < a->n && a->first + i < 8; i++)
            set_slot(base + (int)(a->first + i), hbytes(op, (const uint8_t *)a->v + el * i, el) | 1);
        if (a->first + a->n > 8 && CAP.on) CAP.bad = 4;
        break;
    }
    case R_BeginRendering: set_slot(SL_RENDER, hpacket(op, p, words)); break;
    case R_EndRendering: set_slot(SL_RENDER, 0); break;
    case R_PushConstants: set_slot(SL_PUSHC, hpacket(op, p, words)); break;
    case R_SetBlendConstants: case R_SetCullMode: case R_SetDepthBias: case R_SetDepthBiasEnable: case R_SetDepthCompareOp:
    case R_SetDepthTestEnable: case R_SetDepthWriteEnable: case R_SetFrontFace: case R_SetPrimitiveRestartEnable:
    case R_SetPrimitiveTopology: case R_SetRasterizerDiscardEnable: case R_SetScissorWithCount:
    case R_SetStencilCompareMask: case R_SetStencilOp: case R_SetStencilReference: case R_SetStencilTestEnable:
    case R_SetStencilWriteMask: case R_SetViewportWithCount: case R_VertexInput: case R_PolygonMode:
        // Per-face stencil commands keep one slot each: the replay always sets both faces together.
        set_slot(SL_OP + (int)op, hpacket(op, p, words));
        break;
    default: break;   // draws, binds of sets, barriers, copies, clears, queries: no bound state of a draw
    }
}

int rec_owned(VkCommandBuffer cb) { return OWNED(cb); }
int rec_cap_active(void) { return CAP.on; }

void rec_cap_note_fallback(void) {
    if (CAP.on) CAP.bad = 2;
}

int rec_cap_begin(VkCommandBuffer cb, RecWin *w) {
    if (!OWNED(cb)) return 0;
    CAP.on = 1, CAP.bad = 0, CAP.pos = rec_put_pos(), CAP.w = w, CAP.pk = CAP.np = 0;
    memcpy(w->before, slot, sizeof slot);
    memset(w->written, 0, sizeof w->written);
    w->nwords = w->nreloc = w->npk = w->npair = 0;
    return 1;
}

// Packets a cached draw may hold: the bound state and the draw. Anything else (a pass begun or ended, a barrier, a
// query, a copy) makes the draw one that is recorded again every time.
static int allowed(uint32_t op) {
    switch (op) {
    case R_BindPipeline: case R_BindVertexBuffers2: case R_BindIndexBuffer: case R_BindDescriptorSets: case R_Bool:
    case R_BlendEnable: case R_BlendEquation: case R_WriteMask: case R_SetBlendConstants: case R_SetCullMode:
    case R_SetDepthBias: case R_SetDepthBiasEnable: case R_SetDepthCompareOp: case R_SetDepthTestEnable:
    case R_SetDepthWriteEnable: case R_SetFrontFace: case R_SetPrimitiveRestartEnable: case R_SetPrimitiveTopology:
    case R_SetRasterizerDiscardEnable: case R_SetScissorWithCount: case R_SetStencilCompareMask: case R_SetStencilOp:
    case R_SetStencilReference: case R_SetStencilTestEnable: case R_SetStencilWriteMask: case R_SetViewportWithCount:
    case R_VertexInput: case R_PolygonMode: case R_Draw: case R_DrawIndexed: return 1;
    default: return 0;
    }
}

int rec_cap_end(void) {
    RecWin *w = CAP.w;
    CAP.on = 0;
    CAP.why = CAP.bad;
    if (CAP.bad) return 0;
    uint64_t end = rec_put_pos();
    uint32_t last = 0, ord = 0, pi = 0;
    for (uint64_t at = CAP.pos; at != end;) {
        const uint64_t *hp = rec_ring_word(at);
        uint32_t op = (uint32_t)(*hp & 0xffff), n = (uint32_t)(*hp >> 32);
        at += n;
        if (op == R_PAD) continue;
        if (!allowed(op)) return CAP.why = (int)op, 0;
        last = op;
        uint32_t o = ord++;
        if (op == R_BindDescriptorSets) {   // bound again with the sets of the reusing draw
            while (pi < CAP.np && CAP.pord[pi] == o) pi++;
            continue;
        }
        w->pk_pair[w->npk] = (uint16_t)w->npair;
        for (; pi < CAP.np && CAP.pord[pi] == o; pi++)
            w->psl[w->npair] = CAP.psl[pi], w->ph[w->npair] = CAP.ph[pi], w->npair++;
        if (w->nwords + n > REC_WIN_WORDS || w->npk >= REC_WIN_PACKETS) return CAP.why = 63, 0;
        const uint64_t *p = hp + 1;
        uint64_t lo = (uint64_t)(uintptr_t)p, hi = lo + 8ull * (n - 1);
        uint64_t *d = &w->words[w->nwords];
        d[0] = *hp;
        for (uint32_t i = 0; i + 1 < n; i++) {
            uint64_t v = p[i];
            if (v >= lo && v < hi) {   // a pointer into the packet: kept as its offset, moved when emitted
                if (w->nreloc >= REC_WIN_RELOCS) return CAP.why = 63, 0;
                w->reloc[w->nreloc++] = (uint16_t)(w->nwords + 1 + i);
                v -= lo;
            }
            d[1 + i] = v;
        }
        w->nwords += n, w->npk++;
    }
    if (last != R_Draw && last != R_DrawIndexed) return CAP.why = 62, 0;
    w->pk_pair[w->npk] = (uint16_t)w->npair;
    return 1;
}

int rec_cap_why(void) { return CAP.why; }

// The slots a cached draw relied on without setting them (sl, with the hashes they had) hold the same now.
int rec_slots_match(const uint8_t *sl, const uint64_t *h, int n) {
    for (int i = 0; i < n; i++)
        if (slot[sl[i]] != h[i]) return 0;
    return 1;
}

// Emits w's packets [from, to) (packet indexes) into the ring.
void rec_win_emit(VkCommandBuffer cb, const RecWin *w, int from, int to) {
    int guard = vk_guard_on();
    uint32_t at = 0, r = 0;
    for (int k = 0; k < to; k++) {
        uint64_t h = w->words[at];
        uint32_t op = (uint32_t)(h & 0xffff), n = (uint32_t)(h >> 32);
        int skip = k < from;
        if (!skip && w->pk_pair[k + 1] > w->pk_pair[k]) {   // its state is bound already: left out
            skip = 1;
            for (int i = w->pk_pair[k]; i < w->pk_pair[k + 1] && skip; i++) skip = slot[w->psl[i]] == w->ph[i];
        }
        if (skip) {
            while (r < w->nreloc && w->reloc[r] < at + n) r++;
            at += n;
            continue;
        }
        uint64_t *p = rec_put(op, 8ull * (n - 1));
        memcpy(p, &w->words[at + 1], 8ull * (n - 1));
        for (; r < w->nreloc && w->reloc[r] < at + n; r++) p[w->reloc[r] - at - 1] += (uint64_t)(uintptr_t)p;
        if (guard && op == R_BindPipeline) {
            const A_BindPipeline *a = (const void *)p;
            vk_guard_bound_pipe(cb, a->bp, a->p);
        }
        emitting = 1;
        rec_pub();
        emitting = 0;
        for (int i = w->pk_pair[k]; i < w->pk_pair[k + 1]; i++) slot[w->psl[i]] = w->ph[i];
        at += n;
    }
    rec_dirty_reset();
}
