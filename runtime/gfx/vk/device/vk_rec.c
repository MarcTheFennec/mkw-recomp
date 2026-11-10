// The recorder thread: see vk_rec.h. One producer (the submitting thread, between rec_begin and rec_end) and one
// consumer (the recorder) share a ring of 64-bit words. A packet is a header word (op, length in words) and the
// call's arguments; its pointers point into the packet itself, so the recorder passes the payload to the driver as
// it is. A packet never wraps: one that does not fit before the end is preceded by a pad. The producer publishes head
// after each packet; the recorder makes the calls and advances tail, which the producer waits on when the ring is
// full and in rec_drain. While a part is recorded the recorder spins on head; idle, it sleeps on an event.
// Design: Savage Wind Recomp #189 and LAS Recomp runtime/nvn/vulkan/record.c (https://github.com/peiikai/LASRecomp
// @ a4b53a3, MIT, Copyright (c) 2026 Peiikai); the code is ours.
#define VK_REC_IMPL
#include <immintrin.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "vk.h"
#include "vk_rec_diff.h"
#include "vk_rec_int.h"
#include "vk_rec_reuse.h"
#include "platform/platform.h"

#define RING_WORDS (1u << 21)   // 16 MiB: a part of a submission is ~1-2 MiB of packets in the town
static uint64_t ring[RING_WORDS];
static volatile uint64_t head, tail;   // words published by the producer / consumed by the recorder
static volatile int32_t sleeping;         // the recorder waits for wake_ev
static PlatEvent wake_ev;              // auto-reset
static uint64_t put_h;                 // producer: words written (published up to head)
VkCommandBuffer rec_cb;
int rec_live;
static PlatThread *rec_thread;
static uint64_t last_pk;               // producer: the header word of the last packet (MKW_REC_DIFF)
uint32_t lz[256];
int nlz;
static int rec_prio;                   // the submitting thread's priority, which the recorder takes
_Thread_local RecDirtyState rec_dirty;

void rec_dirty_reset(void) {
    memset(&rec_dirty, 0, sizeof rec_dirty);
}

// Producer statistics, logged by rec_stats_log.
static struct { uint64_t packets, words, drains, fallbacks, drain_ns, full_waits, full_ns, parts, parts_on; } RS_;

static const char *const r_names[] = {
    "pad", "BeginQuery", "BeginRendering", "BindDescriptorSets", "BindIndexBuffer", "BindPipeline", "BindVertexBuffers2",
    "BlitImage", "ClearAttachments", "ClearColorImage", "ClearDepthStencilImage", "CopyBuffer", "CopyBufferToImage",
    "CopyImage", "CopyImageToBuffer", "Dispatch", "DispatchIndirect", "Draw", "DrawIndexed", "DrawIndexedIndirect",
    "DrawIndexedIndirectCount", "DrawIndirect", "DrawIndirectCount", "EndQuery", "EndRendering", "FillBuffer",
    "PipelineBarrier2", "PushConstants", "ResetQueryPool", "ResolveImage", "SetBlendConstants", "SetCullMode",
    "SetDepthBias", "SetDepthBiasEnable", "SetDepthCompareOp", "SetDepthTestEnable", "SetDepthWriteEnable",
    "SetFrontFace", "SetPrimitiveRestartEnable", "SetPrimitiveTopology", "SetRasterizerDiscardEnable",
    "SetScissorWithCount", "SetStencilCompareMask", "SetStencilOp", "SetStencilReference", "SetStencilTestEnable",
    "SetStencilWriteMask", "SetViewportWithCount", "WriteTimestamp", "WriteTimestamp2", "PushTpl", "Push",
    "VertexInput", "Bool", "PolygonMode", "DrawMesh", "BlendEnable", "BlendEquation", "WriteMask", "Marker", "Checkpoint",
};
_Static_assert(sizeof r_names / sizeof *r_names == R_Checkpoint + 1, "r_names follows the R_ enum");
const char *rec_op_name(uint32_t op) { return op <= R_Checkpoint ? r_names[op] : "?"; }

static uint64_t now_ns(void) { return plat_now_ns(); }

// ---- consumer

static void exec_packet(uint32_t op, void *p);

static void rec_main(void *arg) {
    (void)arg;
    plat_thread_priority(rec_prio);
    uint64_t t = 0;
    for (;;) {
        uint64_t h = __atomic_load_n(&head, __ATOMIC_ACQUIRE);
        if (t == h) {
            for (int i = 0; i < 4096 && t == h; i++) _mm_pause(), h = __atomic_load_n(&head, __ATOMIC_ACQUIRE);
            if (t == h) {
                __atomic_store_n(&sleeping, 1, __ATOMIC_SEQ_CST);
                h = __atomic_load_n(&head, __ATOMIC_SEQ_CST);
                if (t == h) plat_event_wait_ns(&wake_ev, 2000000);   // a missed wake costs at most 2 ms
                __atomic_store_n(&sleeping, 0, __ATOMIC_RELAXED);
                continue;
            }
        }
        for (int k = 0; t != h; k++) {
            uint64_t w = ring[t % RING_WORDS];
            uint32_t op = (uint32_t)(w & 0xffff), n = (uint32_t)(w >> 32);
            if (op != R_PAD) exec_packet(op, &ring[t % RING_WORDS + 1]);
            t += n;
            if ((k & 63) == 63) __atomic_store_n(&tail, t, __ATOMIC_RELEASE);
        }
        __atomic_store_n(&tail, t, __ATOMIC_RELEASE);
    }
    return;
}

// ---- producer

static void wake(void) {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (__atomic_load_n(&sleeping, __ATOMIC_RELAXED)) plat_event_set(&wake_ev);
}

static void wait_space(uint64_t need) {
    if (put_h + need - __atomic_load_n(&tail, __ATOMIC_ACQUIRE) <= RING_WORDS) return;
    RS_.full_waits++;
    uint64_t t0 = now_ns();
    __atomic_store_n(&head, put_h, __ATOMIC_RELEASE);
    wake();
    while (put_h + need - __atomic_load_n(&tail, __ATOMIC_ACQUIRE) > RING_WORDS) _mm_pause();
    RS_.full_ns += now_ns() - t0;
}

void *rec_put(uint32_t op, size_t bytes) {
    uint64_t n = 1 + (bytes + 7) / 8, pos = put_h % RING_WORDS;
    if (n > RING_WORDS / 4) rt_fatal("vk_rec: a packet of %llu bytes", (unsigned long long)bytes);
    if (pos + n > RING_WORDS) {
        uint64_t pad = RING_WORDS - pos;
        wait_space(pad);
        ring[pos] = (uint64_t)R_PAD | pad << 32;
        put_h += pad, pos = 0;
    }
    wait_space(n);
    ring[pos] = (uint64_t)op | n << 32;
    // Zeroed: struct padding would carry old ring bytes, and equal calls must give equal packets (MKW_REUSE compares
    // and hashes them; check found 283 hits differing only there, ru4).
    memset(&ring[pos + 1], 0, 8 * (n - 1));
    last_pk = pos;
    put_h += n;
    RS_.packets++, RS_.words += n;
    return &ring[pos + 1];
}

void rec_pub(void) {
    if (rec_diff_on > 0) {
        uint64_t w = ring[last_pk];
        uint32_t op = (uint32_t)(w & 0xffff);
        int draw = (op >= R_Dispatch && op <= R_DrawIndirectCount) || op == R_DrawMesh;
        rec_diff_packet(op, &ring[last_pk + 1], (uint32_t)(w >> 32) - 1, draw, ring, ring + RING_WORDS, lz, nlz);
        nlz = 0;
    }
    if (rec_slots_on) rec_slot_note((uint32_t)(ring[last_pk] & 0xffff), &ring[last_pk + 1], (uint32_t)(ring[last_pk] >> 32) - 1);
    __atomic_store_n(&head, put_h, __ATOMIC_RELEASE);
    if (__atomic_load_n(&sleeping, __ATOMIC_RELAXED)) plat_event_set(&wake_ev);
}

void rec_drain(void) {
    if (!rec_thread || __atomic_load_n(&tail, __ATOMIC_ACQUIRE) == put_h) return;
    uint64_t t0 = now_ns();
    __atomic_store_n(&head, put_h, __ATOMIC_RELEASE);
    wake();
    for (int i = 0; __atomic_load_n(&tail, __ATOMIC_ACQUIRE) != put_h; i++)
        if (i < 20000) _mm_pause();
        else plat_yield();
    RS_.drains++, RS_.drain_ns += now_ns() - t0;
}

void rec_begin(VkCommandBuffer cb) {
    static int mode = -1;
    if (mode < 0) {
        mode = rt_knob("VK_RECORDER", "1");
        if (mode) {
            plat_event_init(&wake_ev, 0, 0);
            rec_prio = plat_thread_priority_get();
            rec_thread = plat_thread_new(rec_main, NULL, "vk recorder");
            if (!rec_thread) {
                rt_log("vk_rec: no recorder thread: the replay makes its Vulkan calls itself");
                mode = 0;
            } else {
                rt_log("vk_rec: the replay's Vulkan commands are made by a recorder thread%s",
                       mode == 2 ? " in the B blocks (MKW_AB)" : "");
            }
        }
    }
    if (rec_diff_on < 0) rec_diff_on = rt_knob("REC_DIFF", "0");
    RS_.parts++;
    rec_dirty_reset();
    if (!RT_KNOB_ON(mode)) return;
    RS_.parts_on++;
    rec_cb = cb;   // the recorder is idle: rec_end drained it
    rec_live = 1;
    if (rec_slots_on) rec_slots_reset();
}

void rec_end(void) {
    if (!rec_live) return;
    rec_drain();
    rec_live = 0;
}

void rec_stats_log(void) {
    if (!(rt_trace & RT_TRACE_PERF) || !RS_.parts) return;
    rt_log("vk_rec: %llu of %llu parts recorded by the recorder: %llu packets (%.1f MiB), %llu drains (%llu for a call "
           "without a packet) waited %.2f ms in all, ring full %llu times (%.2f ms)",
           (unsigned long long)RS_.parts_on, (unsigned long long)RS_.parts, (unsigned long long)RS_.packets,
           RS_.words * 8 / 1048576.0, (unsigned long long)RS_.drains, (unsigned long long)RS_.fallbacks,
           RS_.drain_ns / 1e6, (unsigned long long)RS_.full_waits, RS_.full_ns / 1e6);
    memset(&RS_, 0, sizeof RS_);
}

void rec_fallback(void) { rec_drain(), RS_.fallbacks++, rec_cap_note_fallback(); }
uint64_t rec_put_pos(void) { return put_h; }
const uint64_t *rec_ring_word(uint64_t pos) { return &ring[pos % RING_WORDS]; }
#define put rec_put
#define pub rec_pub
#define tail_copy rec_tail_copy

void *rec_tail_copy(uint8_t **at, const void *arr, size_t bytes) {
    if (!arr || !bytes) return NULL;
    void *d = *at;
    memcpy(d, arr, bytes);
    *at += (bytes + 7) & ~(size_t)7;
    return d;
}
// ---- fixed-size commands: the arguments after cb, as a struct

#define FIXED(name, params, call_args, ...)                                                                             \
    typedef struct { __VA_ARGS__; } A_##name;                                                                          \
    void rc_vkCmd##name params {                                                                                       \
        if (!OWNED(cb)) { vkCmd##name call_args; return; }                                                             \
        A_##name *a = put(R_##name, sizeof *a);                                                                        \
        PACK_##name;                                                                                                    \
        pub();                                                                                                          \
    }

#define PACK_BeginQuery *a = (A_BeginQuery){p, q, f}
FIXED(BeginQuery, (VkCommandBuffer cb, VkQueryPool p, uint32_t q, VkQueryControlFlags f), (cb, p, q, f),
      VkQueryPool p; uint32_t q; VkQueryControlFlags f)
#define PACK_BindIndexBuffer *a = (A_BindIndexBuffer){b, o, t}
FIXED(BindIndexBuffer, (VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkIndexType t), (cb, b, o, t), VkBuffer b;
      VkDeviceSize o; VkIndexType t)
// Not FIXED: MKW_GPU_GUARD notes the bind on both paths (vk_guard.c).
void rc_vkCmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p) {
    if (vk_guard_on()) vk_guard_bound_pipe(cb, bp, p);
    if (!OWNED(cb)) { vkCmdBindPipeline(cb, bp, p); return; }
    A_BindPipeline *a = put(R_BindPipeline, sizeof *a);
    *a = (A_BindPipeline){bp, p};
    pub();
}
#define PACK_Dispatch *a = (A_Dispatch){x, y, z}
FIXED(Dispatch, (VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z), (cb, x, y, z), uint32_t x, y, z)
#define PACK_DispatchIndirect *a = (A_DispatchIndirect){b, o}
FIXED(DispatchIndirect, (VkCommandBuffer cb, VkBuffer b, VkDeviceSize o), (cb, b, o), VkBuffer b; VkDeviceSize o)
#define PACK_Draw *a = (A_Draw){n, i, f, fi}
FIXED(Draw, (VkCommandBuffer cb, uint32_t n, uint32_t i, uint32_t f, uint32_t fi), (cb, n, i, f, fi), uint32_t n, i, f, fi)
#define PACK_DrawIndexed *a = (A_DrawIndexed){n, i, f, v, fi}
FIXED(DrawIndexed, (VkCommandBuffer cb, uint32_t n, uint32_t i, uint32_t f, int32_t v, uint32_t fi), (cb, n, i, f, v, fi),
      uint32_t n, i, f; int32_t v; uint32_t fi)
#define PACK_DrawIndexedIndirect *a = (A_DrawIndexedIndirect){b, o, n, s}
FIXED(DrawIndexedIndirect, (VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t s), (cb, b, o, n, s),
      VkBuffer b; VkDeviceSize o; uint32_t n, s)
// The indirect draws without the recorder ran on the guest thread into the command buffer the recorder thread was
// recording (run97/run98: SIGSEGV in RADV, validation "THREADING ERROR" and "must be issued inside a render pass").
#define PACK_DrawIndexedIndirectCount *a = (A_DrawIndexedIndirectCount){b, o, cb2, co, n, s}
FIXED(DrawIndexedIndirectCount, (VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkBuffer cb2, VkDeviceSize co, uint32_t n,
                                 uint32_t s), (cb, b, o, cb2, co, n, s),
      VkBuffer b; VkDeviceSize o; VkBuffer cb2; VkDeviceSize co; uint32_t n, s)
#define PACK_DrawIndirect *a = (A_DrawIndirect){b, o, n, s}
FIXED(DrawIndirect, (VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t s), (cb, b, o, n, s),
      VkBuffer b; VkDeviceSize o; uint32_t n, s)
#define PACK_DrawIndirectCount *a = (A_DrawIndirectCount){b, o, cb2, co, n, s}
FIXED(DrawIndirectCount, (VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkBuffer cb2, VkDeviceSize co, uint32_t n,
                          uint32_t s), (cb, b, o, cb2, co, n, s),
      VkBuffer b; VkDeviceSize o; VkBuffer cb2; VkDeviceSize co; uint32_t n, s)
#define PACK_EndQuery *a = (A_EndQuery){p, q}
FIXED(EndQuery, (VkCommandBuffer cb, VkQueryPool p, uint32_t q), (cb, p, q), VkQueryPool p; uint32_t q)
#define PACK_EndRendering (void)a
FIXED(EndRendering, (VkCommandBuffer cb), (cb), int unused)
#define PACK_FillBuffer *a = (A_FillBuffer){b, o, n, v}
FIXED(FillBuffer, (VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkDeviceSize n, uint32_t v), (cb, b, o, n, v),
      VkBuffer b; VkDeviceSize o, n; uint32_t v)
#define PACK_ResetQueryPool *a = (A_ResetQueryPool){p, f, n}
FIXED(ResetQueryPool, (VkCommandBuffer cb, VkQueryPool p, uint32_t f, uint32_t n), (cb, p, f, n), VkQueryPool p;
      uint32_t f, n)
typedef struct { float c[4]; } A_SetBlendConstants;
void rc_vkCmdSetBlendConstants(VkCommandBuffer cb, const float c[4]) {
    RecDirtyState *d = rec_dirty_for(cb);
    if (d->bc_valid && !memcmp(d->bc, c, sizeof d->bc) && !rec_cap_active()) return;
    memcpy(d->bc, c, sizeof d->bc);
    d->bc_valid = 1;
    if (!OWNED(cb)) { vkCmdSetBlendConstants(cb, c); return; }
    A_SetBlendConstants *a = put(R_SetBlendConstants, sizeof *a);
    memcpy(a->c, c, sizeof a->c);
    pub();
}
#define PACK_SetCullMode a->v = m
FIXED(SetCullMode, (VkCommandBuffer cb, VkCullModeFlags m), (cb, m), VkCullModeFlags v)
typedef struct { float c, cl, s; } A_SetDepthBias;
void rc_vkCmdSetDepthBias(VkCommandBuffer cb, float c, float cl, float s) {
    RecDirtyState *d = rec_dirty_for(cb);
    float cur[3] = {c, cl, s};
    if (d->db_valid && !memcmp(d->db, cur, sizeof cur) && !rec_cap_active()) return;
    memcpy(d->db, cur, sizeof cur);
    d->db_valid = 1;
    if (!OWNED(cb)) { vkCmdSetDepthBias(cb, c, cl, s); return; }
    A_SetDepthBias *a = put(R_SetDepthBias, sizeof *a);
    *a = (A_SetDepthBias){c, cl, s};
    pub();
}
#define PACK_SetDepthBiasEnable a->v = v
FIXED(SetDepthBiasEnable, (VkCommandBuffer cb, VkBool32 v), (cb, v), VkBool32 v)
#define PACK_SetDepthCompareOp a->v = v
FIXED(SetDepthCompareOp, (VkCommandBuffer cb, VkCompareOp v), (cb, v), VkCompareOp v)
#define PACK_SetDepthTestEnable a->v = v
FIXED(SetDepthTestEnable, (VkCommandBuffer cb, VkBool32 v), (cb, v), VkBool32 v)
#define PACK_SetDepthWriteEnable a->v = v
FIXED(SetDepthWriteEnable, (VkCommandBuffer cb, VkBool32 v), (cb, v), VkBool32 v)
#define PACK_SetFrontFace a->v = v
FIXED(SetFrontFace, (VkCommandBuffer cb, VkFrontFace v), (cb, v), VkFrontFace v)
#define PACK_SetPrimitiveRestartEnable a->v = v
FIXED(SetPrimitiveRestartEnable, (VkCommandBuffer cb, VkBool32 v), (cb, v), VkBool32 v)
#define PACK_SetPrimitiveTopology a->v = v
FIXED(SetPrimitiveTopology, (VkCommandBuffer cb, VkPrimitiveTopology v), (cb, v), VkPrimitiveTopology v)
#define PACK_SetRasterizerDiscardEnable a->v = v
FIXED(SetRasterizerDiscardEnable, (VkCommandBuffer cb, VkBool32 v), (cb, v), VkBool32 v)
#define PACK_SetStencilCompareMask *a = (A_SetStencilCompareMask){f, v}
FIXED(SetStencilCompareMask, (VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t v), (cb, f, v), VkStencilFaceFlags f;
      uint32_t v)
#define PACK_SetStencilOp *a = (A_SetStencilOp){f, x, y, z, d}
FIXED(SetStencilOp, (VkCommandBuffer cb, VkStencilFaceFlags f, VkStencilOp x, VkStencilOp y, VkStencilOp z, VkCompareOp d),
      (cb, f, x, y, z, d), VkStencilFaceFlags f; VkStencilOp x, y, z; VkCompareOp d)
typedef struct { VkStencilFaceFlags f; uint32_t v; } A_SetStencilReference;
void rc_vkCmdSetStencilReference(VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t v) {
    RecDirtyState *d = rec_dirty_for(cb);
    int hit = (f & 3) != 0;
    if ((f & VK_STENCIL_FACE_FRONT_BIT) && (!d->sr_valid[0] || d->sr[0] != v)) hit = 0;
    if ((f & VK_STENCIL_FACE_BACK_BIT) && (!d->sr_valid[1] || d->sr[1] != v)) hit = 0;
    if (hit && !rec_cap_active()) return;
    if (f & VK_STENCIL_FACE_FRONT_BIT) d->sr[0] = v, d->sr_valid[0] = 1;
    if (f & VK_STENCIL_FACE_BACK_BIT) d->sr[1] = v, d->sr_valid[1] = 1;
    if (!OWNED(cb)) { vkCmdSetStencilReference(cb, f, v); return; }
    A_SetStencilReference *a = put(R_SetStencilReference, sizeof *a);
    *a = (A_SetStencilReference){f, v};
    pub();
}
#define PACK_SetStencilTestEnable a->v = v
FIXED(SetStencilTestEnable, (VkCommandBuffer cb, VkBool32 v), (cb, v), VkBool32 v)
#define PACK_SetStencilWriteMask *a = (A_SetStencilWriteMask){f, v}
FIXED(SetStencilWriteMask, (VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t v), (cb, f, v), VkStencilFaceFlags f;
      uint32_t v)
#define PACK_WriteTimestamp *a = (A_WriteTimestamp){s, p, q}
FIXED(WriteTimestamp, (VkCommandBuffer cb, VkPipelineStageFlagBits s, VkQueryPool p, uint32_t q), (cb, s, p, q),
      VkPipelineStageFlagBits s; VkQueryPool p; uint32_t q)
#define PACK_WriteTimestamp2 *a = (A_WriteTimestamp2){s, p, q}
FIXED(WriteTimestamp2, (VkCommandBuffer cb, VkPipelineStageFlags2 s, VkQueryPool p, uint32_t q), (cb, s, p, q),
      VkPipelineStageFlags2 s; VkQueryPool p; uint32_t q)

// ---- the recorder's side

static void exec_packet(uint32_t op, void *p) {
    VkCommandBuffer cb = rec_cb;
    switch (op) {
#define F(name, ...) case R_##name: { A_##name *a = p; vkCmd##name(cb, __VA_ARGS__); break; }
    F(BeginQuery, a->p, a->q, a->f)
    F(BindIndexBuffer, a->b, a->o, a->t)
    F(BindPipeline, a->bp, a->p)
    F(Dispatch, a->x, a->y, a->z)
    F(DispatchIndirect, a->b, a->o)
    F(Draw, a->n, a->i, a->f, a->fi)
    F(DrawIndexed, a->n, a->i, a->f, a->v, a->fi)
    F(DrawIndexedIndirect, a->b, a->o, a->n, a->s)
    F(DrawIndexedIndirectCount, a->b, a->o, a->cb2, a->co, a->n, a->s)
    F(DrawIndirect, a->b, a->o, a->n, a->s)
    F(DrawIndirectCount, a->b, a->o, a->cb2, a->co, a->n, a->s)
    F(EndQuery, a->p, a->q)
    F(FillBuffer, a->b, a->o, a->n, a->v)
    F(ResetQueryPool, a->p, a->f, a->n)
    F(SetBlendConstants, a->c)
    F(SetCullMode, a->v)
    F(SetDepthBias, a->c, a->cl, a->s)
    F(SetDepthBiasEnable, a->v)
    F(SetDepthCompareOp, a->v)
    F(SetDepthTestEnable, a->v)
    F(SetDepthWriteEnable, a->v)
    F(SetFrontFace, a->v)
    F(SetPrimitiveRestartEnable, a->v)
    F(SetPrimitiveTopology, a->v)
    F(SetRasterizerDiscardEnable, a->v)
    F(SetStencilCompareMask, a->f, a->v)
    F(SetStencilOp, a->f, a->x, a->y, a->z, a->d)
    F(SetStencilReference, a->f, a->v)
    F(SetStencilTestEnable, a->v)
    F(SetStencilWriteMask, a->f, a->v)
    F(WriteTimestamp, a->s, a->p, a->q)
    F(WriteTimestamp2, a->s, a->p, a->q)
    F(SetViewportWithCount, a->n, a->v)
    F(SetScissorWithCount, a->n, a->r)
    F(BindDescriptorSets, a->bp, a->l, a->first, a->n, a->s, a->nd, a->d)
    F(BindVertexBuffers2, a->first, a->n, a->b, a->o, a->sz, a->st)
    F(ClearAttachments, a->na, a->a, a->nr, a->r)
    case R_ClearColorImage: { A_ClearImage *a = p; vkCmdClearColorImage(cb, a->i, a->l, &a->v.c, a->n, a->r); break; }
    case R_ClearDepthStencilImage: {
        A_ClearImage *a = p;
        vkCmdClearDepthStencilImage(cb, a->i, a->l, &a->v.ds, a->n, a->r);
        break;
    }
    F(PushConstants, a->l, a->st, a->o, a->n, a->v)
#undef F
    case R_EndRendering: vkCmdEndRendering(cb); break;
    case R_BlitImage: { A_Img *a = p; vkCmdBlitImage(cb, a->s, a->sl, a->d, a->dl, a->n, a->r, a->f); break; }
    case R_CopyImage: { A_Img *a = p; vkCmdCopyImage(cb, a->s, a->sl, a->d, a->dl, a->n, a->r); break; }
    case R_ResolveImage: { A_Img *a = p; vkCmdResolveImage(cb, a->s, a->sl, a->d, a->dl, a->n, a->r); break; }
    case R_CopyBuffer: { A_Xfer *a = p; vkCmdCopyBuffer(cb, (VkBuffer)a->s, (VkBuffer)a->d, a->n, a->r); break; }
    case R_CopyBufferToImage: {
        A_Xfer *a = p;
        vkCmdCopyBufferToImage(cb, (VkBuffer)a->s, (VkImage)a->d, a->l, a->n, a->r);
        break;
    }
    case R_CopyImageToBuffer: {
        A_Xfer *a = p;
        vkCmdCopyImageToBuffer(cb, (VkImage)a->s, a->l, (VkBuffer)a->d, a->n, a->r);
        break;
    }
    case R_PipelineBarrier2: vkCmdPipelineBarrier2(cb, p); break;
    case R_BeginRendering: vkCmdBeginRendering(cb, p); break;
    case R_PushTpl: { A_PushTpl *a = p; a->fn(cb, a->tpl, a->l, a->set, a->data); break; }
    case R_Push: { A_Push *a = p; a->fn(cb, a->bp, a->l, a->set, a->n, a->w); break; }
    case R_VertexInput: { A_VertexInput *a = p; a->fn(cb, a->nb, a->b, a->na, a->v); break; }
    case R_Bool: { A_Bool *a = p; a->fn(cb, a->v); break; }
    case R_PolygonMode: { A_PolygonMode *a = p; a->fn(cb, a->m); break; }
    case R_DrawMesh: { A_DrawMesh *a = p; a->fn(cb, a->x, a->y, a->z); break; }
    case R_Marker: { A_Marker *a = p; a->fn(cb, a->stage, a->b, a->o, a->v); break; }
    case R_Checkpoint: { A_Checkpoint *a = p; a->fn(cb, a->m); break; }
    case R_BlendEnable: { A_Targets *a = p; ((PFN_vkCmdSetColorBlendEnableEXT)a->fn)(cb, a->first, a->n, a->v); break; }
    case R_BlendEquation: { A_Targets *a = p; ((PFN_vkCmdSetColorBlendEquationEXT)a->fn)(cb, a->first, a->n, a->v); break; }
    case R_WriteMask: { A_Targets *a = p; ((PFN_vkCmdSetColorWriteMaskEXT)a->fn)(cb, a->first, a->n, a->v); break; }
    default: rt_fatal("vk_rec: unknown packet %u", op);
    }
}
