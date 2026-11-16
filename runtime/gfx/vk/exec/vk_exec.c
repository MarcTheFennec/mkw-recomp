// The op-stream replay core of the Vulkan back end: walks the command handles of
// one submission into its command buffer, keeps render targets and fixed-function state, opens and closes dynamic
// rendering passes and records clears and barriers. Draws, binds and dispatches go to vk_exec_op_draw
// (vk_exec_ops.c); copies, counters, events and conditional rendering to vk_exec_op_xfer (vk_exec_xfer.c). Images
// stay in VK_IMAGE_LAYOUT_GENERAL (vk_texture.c); full memory barriers order passes, clears and whatever those two
// record between them.
//
// The op loop (END / BEGIN / JUMP / CALL, which ops are pure state, the state defaults, render targets kept until they
// change, clears skipped while rendering is disabled) follows MacNativeGames
// games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m:10588-10700, 7409-7460, 5449-5457
// (https://github.com/ly05010419/MacNativeGames @ b75be8cfa704df3701e86130c8ced268e79d6b11, MIT, Copyright (c) 2026
// jhaizhou). Pass begin and end (GENERAL-layout attachments, LOAD/STORE, render area from the smallest attachment, a
// full barrier before each pass), the scissor clip and attachment clears follow LAS Recomp
// runtime/nvn/vulkan/exec.c:855-1149, 3553-3778 (https://github.com/peiikai/LASRecomp @
// da35b83a9611d046a9a681aae668d7085954d37f, MIT, Copyright (c) 2026 Peiikai).
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "nvn_api.h"
#include "nvn_tex.h"
#include "tex.h"
#include "vk.h"
#include "vk_exec.h"
#include "platform/platform.h"

#include "vk_exec_internal.h"
#include "vk_rec_diff.h"
#include "vk_reuse.h"
// Task/mesh draws (VK_EXT_mesh_shader): drawn by default since mesh1 (01/11/2026: 40 s of race without a hang; with
// them the grid's racers show and the cars stop flickering, live23/24). MKW_MESH=0 counts them without drawing.
static int mesh_on(void) {
    static int on = -1;
    if (on < 0) on = (vk_ctx()->has_mesh || vk_mesh_cs_on()) && !(cfg_get("MKW_MESH") && !atoi(cfg_get("MKW_MESH")));
    return on;
}

#define WEAK __attribute__((weak))
#define CALL_DEPTH VK_EXEC_CALL_DEPTH

static VkExec X;
static int x_ready;
// The replay state this thread records with: X inside vk_exec_record, a chunk's copy on MKW_REPLAY_PAR's threads
// (vk_exec_inline_begin, vk_exec_recording_fence); NULL elsewhere.
static _Thread_local VkExec *x_cur;
static _Thread_local VkExec *x_here_ex;   // X while this thread is in vk_exec_record
static _Thread_local uint64_t x_pos;      // ops handed to vk_exec_op in this submission (a chunk starts at its first)

uint64_t vk_exec_op_pos(void) { return x_pos; }
void vk_exec_op_pos_set(uint64_t pos) { x_pos = pos; }

// ---- MKW_TRACE=gfx frame counters

VkExecFrame vk_exec_frame_all;
_Thread_local VkExecFrame *vk_exec_frame_cur = &vk_exec_frame_all;

void vk_exec_frame_add(const VkExecFrame *f) {
    uint64_t *d = (uint64_t *)&vk_exec_frame_all;
    const uint64_t *s = (const uint64_t *)f;
    for (size_t i = 0; i < sizeof *f / sizeof *d; i++) d[i] += s[i];
}
static struct { const uint8_t *mem; uint32_t w, h, fmt, n; } fw[64];   // textures written this frame
static int nfw;

uint64_t vk_exec_recording_fence(void) { return x_cur ? x_cur->fence : 0; }

void vk_exec_frame_written(const tex_t *t) {
    if (!(rt_trace & RT_TRACE_GFX) || !t) return;
    for (int i = 0; i < nfw; i++)
        if (fw[i].mem == t->mem) { fw[i].n++; return; }
    if (nfw == 64) return;
    fw[nfw].mem = t->mem, fw[nfw].w = t->d.width, fw[nfw].h = t->d.height, fw[nfw].fmt = (uint32_t)t->d.format;
    fw[nfw++].n = 1;
}

uint64_t vk_exec_presented;

void vk_exec_frame_log(const tex_t *p, uint64_t frame) {
    vk_exec_presented = frame;
    vk_mem_lost_log(frame);
    if (!(rt_trace & RT_TRACE_GFX)) return;
    VkExecFrame *f = &vk_exec_frame_all;
    int hit = 0;
    char list[512] = "";
    size_t o = 0;
    for (int i = 0; i < nfw; i++) {
        if (p && fw[i].mem == p->mem) hit = 1;
        if (o < sizeof list - 48)
            o += (size_t)snprintf(list + o, sizeof list - o, " %p:%ux%u/%#x*%u", (const void *)fw[i].mem, fw[i].w,
                                  fw[i].h, fw[i].fmt, fw[i].n);
    }
    rt_log("frame %llu: submits %llu passes %llu draws %llu/%llu clears %llu copies %llu dispatches %llu; present %p "
           "%ux%u/%#x written %d; feedback draws %llu barriers %llu restarts %llu layouts %llu; skips off %llu scissor %llu "
           "program %llu no-vs %llu shader-missing %llu pipe-wait %llu pipe-failed %llu mesh %llu; targets%s",
           (unsigned long long)frame, (unsigned long long)f->submits,
           (unsigned long long)f->passes, (unsigned long long)f->draws, (unsigned long long)f->draws_seen,
           (unsigned long long)f->clears, (unsigned long long)f->copies, (unsigned long long)f->dispatches,
           p ? (const void *)p->mem : NULL, p ? p->d.width : 0, p ? p->d.height : 0, p ? (uint32_t)p->d.format : 0,
           hit, (unsigned long long)f->fb_draws, (unsigned long long)f->fb_barriers, (unsigned long long)f->fb_restarts,
           (unsigned long long)f->fb_layouts, (unsigned long long)f->skip[0], (unsigned long long)f->skip[1],
           (unsigned long long)f->skip[2], (unsigned long long)f->skip[3], (unsigned long long)f->skip[4],
           (unsigned long long)f->skip[5], (unsigned long long)f->skip[6], (unsigned long long)f->skip[7], list);
    memset(f, 0, sizeof *f);
    nfw = 0;
}

WEAK void vk_exec_op_draw(VkExec *ex, uint32_t op, const void *payload) {
    (void)ex, (void)payload;
    rt_fatal("vk: op %s is not replayed yet", nvn_op_name(op));
}

WEAK void vk_exec_op_xfer(VkExec *ex, uint32_t op, const void *payload) {
    (void)ex, (void)payload;
    rt_fatal("vk: op %s is not replayed yet", nvn_op_name(op));
}

void exec_warn_once(int key, const char *fmt, ...) {
    static volatile int32_t seen[W_COUNT];
    if (__atomic_exchange_n(&seen[key], 1, __ATOMIC_RELAXED)) return;
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    rt_log("vk: %s (logged once)", msg);
}

// NVN defaults of the replay state (nvn_mtl.m).
static void state_reset(VkExec *ex) {
    memset(ex, 0, sizeof *ex);
    ex->st_vmask[0] = ex->st_vmask[1] = ex->st_wmask[0] = ex->st_wmask[1] = 0xff;
    ex->depth_range[1] = 1.0f;
    vk_pipe_state_defaults(&ex->pipe);
    ex->barrier_at = ex->last_dispatch = ex->clear_run_at = -1;
}

// ---- passes and barriers

static int has_image(const VkImage *list, int n, VkImage img) {
    for (int i = 0; i < n; i++)
        if (list[i] == img) return 1;
    return 0;
}

static VkImageMemoryBarrier2 layout_barrier(VkImage img, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to) {
    return (VkImageMemoryBarrier2){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                   .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                                   .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                   .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                   .oldLayout = from, .newLayout = to, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
                                   .subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
}

// Full barrier. The images in the feedback layout become want[] (pass begin) or none (every other barrier): the others
// go back to GENERAL and the new ones leave it, in the same command.
// MKW_PASS_NOBARRIER (default 0; "ab": in the B blocks): no memory barrier before a pass. A measurement only, of what
// that barrier costs the GPU (~137 passes a race frame, rt-y2); without it passes may read stale results.
static int pass_nobarrier = -1;
int pt_on = -1;   // MKW_PASS_TIMING (vk_passtime.c)
_Thread_local ExecDC DC;   // MKW_DEPTH_CLEAR_ALL (vk_exec_clear.c dc_layered); per recording thread
uint64_t dc_cnt[4];

static void barrier_to(VkExec *ex, const VkImage *want, const VkImageAspectFlags *wasp, int nwant, int pass) {
    vk_exec_pass_end(ex);
    VkImageMemoryBarrier2 ib[18];
    uint32_t n = 0;
    for (int i = 0; i < ex->nfbl; i++)
        if (!has_image(want, nwant, ex->fbl_img[i]))
            ib[n++] = layout_barrier(ex->fbl_img[i], ex->fbl_aspect[i], VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT,
                                     VK_IMAGE_LAYOUT_GENERAL);
    for (int i = 0; i < nwant; i++)
        if (!has_image(ex->fbl_img, ex->nfbl, want[i]))
            ib[n++] = layout_barrier(want[i], wasp[i], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT);
    for (int i = 0; i < nwant; i++) ex->fbl_img[i] = want[i], ex->fbl_aspect[i] = wasp[i];
    ex->nfbl = nwant;
    if (ex->barrier_at == ex->recorded && !n) return;
    if (pass && pass_nobarrier < 0) {
        pass_nobarrier = rt_knob("PASS_NOBARRIER", "0");
        if (pass_nobarrier) rt_log("vk: no memory barrier before passes%s (MKW_PASS_NOBARRIER, a measurement)",
                                   pass_nobarrier == 2 ? " in the B blocks" : "");
    }
    if (pass && !n && pass_nobarrier && RT_KNOB_ON(pass_nobarrier)) return;
    vk_exec_frame.fb_layouts += n;
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb,
                           .imageMemoryBarrierCount = n, .pImageMemoryBarriers = ib};
    vkCmdPipelineBarrier2(ex->cb, &di);
    ex->barrier_at = ++ex->recorded;
}

void vk_exec_barrier(VkExec *ex) { barrier_to(ex, NULL, NULL, 0, 0); }

void vk_exec_pass_begin(VkExec *ex) {
    if (ex->in_pass) return;
    resolve_rt(ex);
    // Learned targets (VkTexImage.fb_learned) are attached in the feedback layout
    VkImage want[9];
    VkImageAspectFlags wasp[9];
    int nwant = 0;
    uint32_t fbl = 0;
    for (uint32_t i = 0; i < 9; i++) {
        const VkExecTarget *t = i < 8 ? &ex->color[i] : &ex->depth;
        if ((i < 8 && i >= ex->pass_ncolor) || !t->image || !t->img || !t->img->fb_learned) continue;
        fbl |= 1u << i;
        if (has_image(want, nwant, t->image)) continue;
        want[nwant] = t->image, wasp[nwant++] = i < 8 ? VK_IMAGE_ASPECT_COLOR_BIT : t->img->aspect;
    }
    barrier_to(ex, want, wasp, nwant, 1);   // passes, clears, copies and dispatches see each other's results
    ex->fbl_mask = fbl;
    ex->fb_dirty = 0;
    ex->pass_serial++;
    VkRenderingAttachmentInfo ca[8], da = {0}, sa = {0};
    for (uint32_t i = 0; i < ex->pass_ncolor; i++) {
        const VkExecTarget *t = &ex->color[i];
        ca[i] = (VkRenderingAttachmentInfo){.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        if (!t->image) continue;
        ca[i].imageView = view_of(ex, t);
        ca[i].imageLayout = fbl >> i & 1 ? VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT : VK_IMAGE_LAYOUT_GENERAL;
        ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;    }
    const VkExecTarget *d = &ex->depth;
    if (d->image) {
        VkRenderingAttachmentInfo a = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = view_of(ex, d),
                                       .imageLayout = fbl >> 8 & 1 ? VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT
                                                                   : VK_IMAGE_LAYOUT_GENERAL,
                                       .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        if (d->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) da = a;
        if (d->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) sa = a;
    }
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {ex->pass_w, ex->pass_h}},
                          .layerCount = ex->pass_layers, .colorAttachmentCount = ex->pass_ncolor,
                          .pColorAttachments = ca,
                          .pDepthAttachment = ex->pass_depth ? &da : NULL,
                          .pStencilAttachment = ex->pass_stencil ? &sa : NULL};
    if (pt_on > 0) vk_pt_mark(ex->cb, (uint32_t)ex->pass_w << 16 | (ex->pass_h & 0xffff));
    ex->pass_crumb = vk_crumb_begin(ex->cb, VK_CRUMB_PASS, ex->pass_w, ex->pass_h, ex->pass_layers, ex->pass_ncolor,
                                    ex->pass_color, ex->pass_depth ? ex->pass_depth : ex->pass_stencil);
    vkCmdBeginRendering(ex->cb, &ri);
    if (vk_dump_armed) vk_dump_pass_begin(ex);
    vk_exec_frame.passes++;
    ex->recorded++;
    ex->in_pass = 1;
}

// Set while a pass ends only to resume (maybe_split's new command buffer, an inline command such as MKW_DUMP_DRAWS's
// snapshots or MKW_TEX_ALIAS's copies): the targets are half drawn, MKW_RTSTATS leaves them for the real end of the pass.
static _Thread_local int split_ending;

void vk_exec_pass_end(VkExec *ex) {
    if (!ex->in_pass) return;
    vk_exec_oq_pass_end(ex);
    if (vk_dump_armed) vk_dump_pass_close(ex);
    vkCmdEndRendering(ex->cb);
    vk_crumb_end(ex->cb, VK_CRUMB_PASS, ex->pass_crumb);
    ex->pass_crumb = 0;
    if (pt_on > 0) vk_pt_mark(ex->cb, 0);
    ex->recorded++;
    ex->in_pass = 0;
    vk_ind_read_pass_end(ex);
    if (vk_dump_armed) vk_dump_pass_end(ex);
    if (vk_rtstats_on && !split_ending) vk_rtstats_pass_end(ex);
    uint64_t stamp = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + 1;
    for (int i = 0; i < 8; i++)
        if (ex->color[i].image) {
            const VkExecTarget *t = &ex->color[i];
            vk_texture_gpu_written(t->tex), tfr_note(t, stamp);
            vk_sparse_written(t->image, t->level, t->base_layer, t->layers, 0, 0, 0, 0);
        }
    if (ex->depth.image) vk_texture_gpu_written(ex->depth.tex), tfr_note(&ex->depth, stamp);
    if (DC.pending && ex->depth.image == DC.img)   // the pass wrote those layers: not cleared ahead any more
        DC.pending &= ~(((ex->depth.layers >= 32 ? 0u : 1u << ex->depth.layers) - 1u) << ex->depth.base_layer);
}

VkCommandBuffer vk_exec_inline_begin(int *reopen) {
    VkExec *ex = x_cur;
    *reopen = 0;
    if (!ex || !ex->cb) return VK_NULL_HANDLE;
    *reopen = ex->in_pass;
    split_ending = ex->in_pass;
    vk_exec_barrier(ex);   // ends the pass; feedback-layout targets go back to GENERAL
    split_ending = 0;
    return ex->cb;
}

void vk_exec_inline_end(int reopen) {
    x_cur->recorded++;
    if (reopen) vk_exec_pass_begin(x_cur);
}

typedef struct { VkBuffer b; VmaAllocation a; } InlineBuf;
static void free_inline_buf(void *arg) {   // completion thread
    InlineBuf *t = arg;
    vmaDestroyBuffer(vk_ctx()->vma, t->b, t->a);
    free(t);
}

VkBuffer vk_exec_inline_buffer(VkDeviceSize size) {
    InlineBuf *t = calloc(1, sizeof *t);
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                             .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (!t || vmaCreateBuffer(vk_ctx()->vma, &bi, &ai, &t->b, &t->a, NULL) != VK_SUCCESS) { free(t); return VK_NULL_HANDLE; }
    vk_completion_add(x_cur ? x_cur->fence : X.fence, free_inline_buf, t);
    return t->b;
}

VkRect2D vk_exec_scissor(const VkExec *ex) {
    int64_t w = ex->pass_w, h = ex->pass_h;
    if (!ex->has_scissor) return (VkRect2D){{0, 0}, {(uint32_t)w, (uint32_t)h}};
    int64_t x0 = ex->scissor.x, y0 = ex->scissor.y;
    int64_t x1 = x0 + ex->scissor.w, y1 = y0 + ex->scissor.h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    if (x1 <= x0 || y1 <= y0) return (VkRect2D){{0, 0}, {0, 0}};
    return (VkRect2D){{(int32_t)x0, (int32_t)y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}};
}

// ---- the op loop

static void set_rt(VkExec *ex, const p_set_rt *r) {
    if (!memcmp(&ex->rt, r, sizeof *r)) return;   // the same targets again: the pass goes on
    vk_exec_pass_end(ex);
    ex->rt = *r;
    ex->rt_resolved = 0;
}

// ---- command buffer parts
// NVN starts the GPU as soon as commands are submitted; one Vulkan command buffer per submission starts it only after
// the whole replay. Between passes, once MKW_SPLIT_DRAWS (64; 0 off) draws and dispatches were recorded since the
// last part, the part goes to the queue (vk_sync_split) and the replay continues in a new command buffer. Only
// command-buffer state is lost there (bindings and dynamic state: vk_exec_draw_new_cb, ex->dirty); barriers, layouts and
// queries are not: a barrier's first scope is everything earlier in submission order, image layouts belong to the image,
// and occlusion queries end with their pass. Fences, completion callbacks and reports stay per submission: only the last
// part signals the value. nxnative commits every 64 draws between passes; MacNativeGames after 30, then every 40.
// A part also spans at least MKW_SPLIT_US (2000) of replay time: a split costs about 40 us of the render thread
// (vkQueueSubmit2, a command buffer, the mirror prologue), and a fight frame of 2,000 draws made 15 parts on the 7900X;
// a millisecond of recording is a fraction of a millisecond of GPU work, so the GPU still starts early. On the Radeon
// 660M each split also syncs the shadow pools (~0.2 ms), ~17 splits a race frame on the replay thread, the frame's
// critical path: 2000 against 1000, -2.8 ms a frame with the scene balanced (sp2/sp2i, 07/11/2026; on 01/11, with
// the per-submit wait, 1000 was best: rt-w1, rt-w2).
// Inside a pass too with MKW_SPLIT_IN_PASS=1: MKW draws a race frame in long passes, and splitting only
// between them leaves ~1-2 parts per submission. Passes load and store every attachment (clears are separate
// commands), so the pass ends before the split and begins again after it, as the feedback restart does; an occlusion
// query open there closes into a segment of its own. Default on with the asynchronous replay, whose thread pays for
// the splits while the game builds its next frame: race 40.0 -> 32.1 and 45.4 -> 35.3 ms with both (rt-t*). Off on the
// submitting thread, where it did not help (rt-l2, rt-r2).
static int split_in_pass, split_on;   // split_on: MKW_SPLIT (default 1; "ab" splits in the B blocks only)
static int split_n = -1, split_draws, part_start;
static uint64_t split_ns, split_ns_b, part_t0;   // split_ns_b: MKW_SPLIT_US_B, the B blocks' (MKW_AB)
static uint64_t n_parts, n_subs, n_in_pass;

static void maybe_split(VkExec *ex) {
    if (split_n < 0) {
        split_n = cfg_int("MKW_SPLIT_DRAWS");
        if (split_n < 0) split_n = 0;
        int us = cfg_int("MKW_SPLIT_US");
        split_ns = us > 0 ? (uint64_t)us * 1000 : 0;
        int ub = cfg_get("MKW_SPLIT_US_B") ? atoi(cfg_get("MKW_SPLIT_US_B")) : us;
        split_ns_b = ub > 0 ? (uint64_t)ub * 1000 : 0;
        split_in_pass = rt_knob("SPLIT_IN_PASS", gfx_replay_async() ? "1" : "0");
        split_on = rt_knob("SPLIT", "1");
        rt_log("vk: replay command buffer split every %d draws %s, parts of at least %d us%s", split_n,
               split_in_pass == 2 ? "(inside passes too in the B blocks)" : split_in_pass ? "(inside passes too)" : "between passes",
               us > 0 ? us : 0, split_n ? "" : " (off)");
    }
    if (!split_n || !RT_KNOB_ON(split_on) || (ex->in_pass && !RT_KNOB_ON(split_in_pass)) || split_draws < split_n) return;
    uint64_t min_ns = rt_ab() ? split_ns_b : split_ns;
    if (min_ns && kern_now_ns() - part_t0 < min_ns) return;   // split_draws stays: the next draw asks again
    split_draws = 0;
    if (ex->recorded == part_start) return;
    int reopen = ex->in_pass;
    if (reopen) split_ending = 1, vk_exec_pass_end(ex), split_ending = 0, n_in_pass++;
    ex->cb = vk_sync_split();
    part_start = ex->recorded;
    part_t0 = kern_now_ns();
    ex->dirty = VK_EXEC_DIRTY_ALL;
    vk_exec_draw_new_cb();
    n_parts++;
    if (reopen) vk_exec_pass_begin(ex);
}

// A draw or dispatch recorded on the replay thread outside walk (MKW_REPLAY_PAR's serial head, vk_exec_par.c): the
// same split check, so the head's first part goes to the GPU as early as the serial replay's.
void vk_exec_drew(VkExec *ex) {
    if (++split_draws >= split_n) maybe_split(ex);
}

// MKW_TRACE=draw: the replay's time per op (TSC ticks, the call's own handler only), logged with the parts line of
// MKW_TRACE=perf: where the recording's CPU goes (rt-h2: ~730 ms per s of wall clock in a race). Not part of perf: its
// clock reads were a third of bind_resources' own samples (rt-x2, perf record).
static int op_timing = -1;
enum { OPX_BEGIN = OP_COUNT_, OPX_SPLIT, OPX_END, OPX_HIT, OPX_CAP, OPX_N };   // the submission's start and end, the
                                                                              // splits, draws MKW_REUSE served, captured
static uint64_t op_tsc[OPX_N], op_cnt[OPX_N], op_t0, op_ns0;
static _Thread_local int op_hit;   // the op being walked was a draw MKW_REUSE served (OPX_HIT) or captured (OPX_CAP)
static const char *opx_name(int i) {
    return i == OPX_BEGIN ? "(start: mirrors, usnap)" : i == OPX_SPLIT ? "(splits)" : i == OPX_END ? "(end of the walk)"
           : i == OPX_HIT ? "(draws reused)" : i == OPX_CAP ? "(draws captured)" : nvn_op_name((uint32_t)i);
}

void vk_exec_op_timing_log(void) {
    if (op_timing <= 0) return;
    uint64_t t = plat_cycles(), ns = kern_now_ns(), span = op_t0 ? t - op_t0 : 0, all = 0;
    double us_per_tick = span ? (double)(ns - op_ns0) / 1e3 / (double)span : 0;   // the TSC's rate, from this span
    op_t0 = t, op_ns0 = ns;
    int ord[OPX_N];
    for (int i = 0; i < OPX_N; i++) ord[i] = i, all += op_tsc[i];
    for (int i = 1; i < OPX_N; i++)   // by time, a few dozen entries
        for (int j = i; j > 0 && op_tsc[ord[j]] > op_tsc[ord[j - 1]]; j--) { int x = ord[j]; ord[j] = ord[j - 1], ord[j - 1] = x; }
    char buf[900];
    int k = 0;
    for (int i = 0; i < 10 && op_tsc[ord[i]] && span; i++)
        k += snprintf(buf + k, sizeof buf - (size_t)k, "%s %s %.1f%% (%.2f us x %llu)", i ? ";" : "", opx_name(ord[i]),
                      100.0 * (double)op_tsc[ord[i]] / (double)span,
                      (double)op_tsc[ord[i]] / (double)op_cnt[ord[i]] * us_per_tick,
                      (unsigned long long)op_cnt[ord[i]]);
    if (span) rt_log("perf: replay ops, share of the wall clock (%.1f%% in all):%s", 100.0 * (double)all / (double)span, buf);
    if (span && vk_exec_draw_timing(buf, sizeof buf, us_per_tick)) rt_log("perf: prepare_draw per draw, us:%s", buf);
    memset(op_tsc, 0, sizeof op_tsc), memset(op_cnt, 0, sizeof op_cnt);
}

// The fixed-function state ops, which record nothing: 1 when op is one.
int vk_exec_state_op(VkExec *ex, uint32_t op, const void *a) {
    switch (op) {
    case OP_VIEWPORTS: {
        const p_viewports *v = a;
        for (int i = 0; i < v->n && v->first + i < 16; i++)
            if (v->first + i >= 0) memcpy(ex->vp[v->first + i], v->v[i], sizeof ex->vp[0]);
        ex->has_vp = 1;
        ex->dirty |= VK_EXEC_DIRTY_VIEWPORT;
        break;
    }
    case OP_SCISSOR:
        ex->scissor = *(const p_scissor *)a;
        ex->has_scissor = 1;
        ex->dirty |= VK_EXEC_DIRTY_SCISSOR;
        break;
    case OP_DEPTH_RANGE: {
        const p_depth_range *r = a;
        ex->depth_range[0] = r->n, ex->depth_range[1] = r->f;
        ex->dirty |= VK_EXEC_DIRTY_VIEWPORT;
        break;
    }
    case OP_STENCIL: {
        const p_stencil *s = a;
        for (int f = 0; f < 2; f++) {
            if (!(s->face & (1 << f))) continue;
            if (s->which == 0) ex->st_vmask[f] = (uint32_t)s->value_mask;
            else if (s->which == 1) ex->st_wmask[f] = (uint32_t)s->mask;
            else ex->st_ref[f] = (uint32_t)s->ref;
        }
        ex->dirty |= VK_EXEC_DIRTY_STENCIL;
        break;
    }
    case OP_POLY_OFFSET:
        ex->poly_offset = *(const p_poly_offset *)a;
        ex->dirty |= VK_EXEC_DIRTY_DEPTH_BIAS;
        break;
    case OP_BLEND_COLOR:
        memcpy(ex->blend_color, ((const p_blend_color *)a)->rgba, sizeof ex->blend_color);
        ex->dirty |= VK_EXEC_DIRTY_BLEND_CONST;
        break;
    case OP_ALPHA_REF: ex->alpha_ref = ((const p_alpha_ref *)a)->ref; break;
    case OP_WAIT_EVENT: break;   // the order of the queues' work is nvn_qorder.c's, at submit
    case OP_DEPTH_CLAMP: ex->pipe.depth_clamp = ((const p_flag *)a)->value != 0; break;
    case OP_RASTER_DISCARD: ex->pipe.raster_discard = ((const p_flag *)a)->value != 0; break;
    case OP_PRIM_RESTART: ex->prim_restart = *(const p_prim_restart *)a; break;
    case OP_COPY_STRIDE: ex->copy_stride = ((const p_copy_stride *)a)->stride; break;
    case OP_SET_TEX_POOL: ex->tex_pool = ((const p_set_pool *)a)->pool, ex->tex_serial++; break;
    case OP_SET_SAMP_POOL: ex->samp_pool = ((const p_set_pool *)a)->pool; break;
    default: return 0;
    }
    return 1;
}

// OP_QUEUE (nvn_qorder.c): the game's queues take turns inside one submission at their event waits, and each goes on
// with the replay state it had (Hookshot found in another MKW recomp that one queue's draws otherwise went on with what
// the other had set). A queue first seen in the submission starts from the state the replay is at, as every
// submission does. The switch ends the pass and puts a full barrier between the two queues' work: the wait it stands
// for orders the other queue's writes before what follows.
static VkExecQueues QS;

// MKW_QORDER_LOG=N (diagnosis): the first N queue switches, each with what the queue before it ran since its turn
// began (draws, dispatches, ops), to tell which stages the queues pass each other.
static void queue_switch_log(uint64_t id) {
    static int left = -1;
    static uint64_t d0, c0, o0;
    if (left < 0) left = cfg_get("MKW_QORDER_LOG") ? atoi(cfg_get("MKW_QORDER_LOG")) : 0;
    if (!left) return;
    left--;
    if (!QS.cur) o0 = 0;   // (op positions count from the submission's start)
    rt_log("vk: queue %#llx after %#llx: %llu draws, %llu dispatches, %llu ops in its turn (fence %llu)",
           (unsigned long long)id, (unsigned long long)QS.cur,
           (unsigned long long)(vk_exec_frame.draws_seen - d0), (unsigned long long)(vk_exec_frame.dispatches - c0),
           (unsigned long long)(x_pos - o0), (unsigned long long)vk_exec_recording_fence());
    d0 = vk_exec_frame.draws_seen, c0 = vk_exec_frame.dispatches, o0 = x_pos;
}

int vk_exec_queue_swap(VkExecQueues *t, VkExec *ex, uint64_t id) {
    if (id == t->cur) return 0;
    int from = -1, to = -1;
    for (int i = 0; i < t->n; i++) {
        if (t->q[i].id == t->cur) from = i;
        if (t->q[i].id == id) to = i;
    }
    if (t->cur && from < 0 && t->n < VK_EXEC_QUEUES) from = t->n++, t->q[from].id = t->cur;
    if (from >= 0) t->q[from].st = *ex;
    t->cur = id;
    if (to < 0) return 1;   // a queue first seen goes on from ex
    VkExec keep = *ex;
    *ex = t->q[to].st;
    // What belongs to the command buffer being recorded, not to the queue: kept.
    ex->cb = keep.cb, ex->fence = keep.fence, ex->recorded = keep.recorded, ex->in_pass = 0;
    ex->views = keep.views, ex->nviews = keep.nviews, ex->cap_views = keep.cap_views;
    ex->barrier_at = keep.barrier_at, ex->last_dispatch = keep.last_dispatch, ex->clear_run_at = keep.clear_run_at;
    ex->nclr = keep.nclr, memcpy(ex->clr, keep.clr, sizeof ex->clr);
    memcpy(ex->fbl_img, keep.fbl_img, sizeof ex->fbl_img), memcpy(ex->fbl_aspect, keep.fbl_aspect, sizeof ex->fbl_aspect);
    ex->nfbl = keep.nfbl, ex->fbl_mask = keep.fbl_mask, ex->fb_dirty = keep.fb_dirty, ex->fb_fs = NULL;
    ex->tex_serial = keep.tex_serial + 1, ex->pass_serial = keep.pass_serial + 1;
    ex->dirty = VK_EXEC_DIRTY_ALL, ex->rt_resolved = 0;
    return 2;
}

static void queue_switch(VkExec *ex, uint64_t id) {
    if (id == QS.cur) return;
    queue_switch_log(id);
    vk_exec_pass_end(ex);
    vk_exec_barrier(ex);
    if (vk_exec_queue_swap(&QS, ex, id) < 2) return;
    dc_drop("a queue switch");
    vk_exec_draw_new_cb();
}

// One op that is not END, BEGIN, JUMP or CALL: 1 when it was a draw or dispatch (the split check follows those).
int vk_exec_op(VkExec *ex, const op_hdr *h) {
    const void *a = nvn_op_payload(h);
    x_pos++;
    int ptm = pt_on > 0 && !ex->in_pass;   // MKW_PASS_TIMING: the op's GPU time, outside passes
    if (ptm) vk_pt_mark(ex->cb, 1u << 30 | h->op);
    int drew = 0;
    switch (h->op) {
    case OP_SET_RT: set_rt(ex, a); break;
    case OP_CLEAR_COLOR: clear_color(ex, a); break;
    case OP_CLEAR_DS: clear_ds(ex, a); break;
    case OP_BARRIER: {   // #43: the GPU hung between marks, where only barriers and such were recorded
        uint32_t crumb = vk_crumb_op(ex->cb, "barrier", 0, 0);
        vk_exec_barrier(ex);
        vk_crumb_end(ex->cb, VK_CRUMB_OP, crumb);
        break;
    }
    case OP_DRAW_MESH:   // task/mesh pipelines (VK_EXT_mesh_shader) with MKW_MESH=1; else counted, not drawn
        vk_exec_frame.draws_seen++;
        if (mesh_on()) vk_exec_op_draw(ex, h->op, a);
        else vk_exec_frame.skip[VK_SKIP_MESH]++;
        break;
    case OP_DRAW: case OP_DRAW_IDX:   // MKW_REUSE: from the cache when an earlier submission recorded the same draw
        vk_exec_frame.draws_seen++, drew = 1;
        if (ru_on > 0 && ru_draw(ex, h->op, a)) { op_hit = OPX_HIT; break; }
        if (ru_cap) op_hit = OPX_CAP;
        vk_exec_op_draw(ex, h->op, a);
        if (ru_cap) ru_draw_end(ex);
        break;
    case OP_DISPATCH: case OP_UPDATE_UB: case OP_DRAW_IDX_IND: case OP_DISPATCH_IND:
        if (h->op == OP_DRAW_IDX_IND) vk_exec_frame.draws_seen++, drew = 1;
        if (h->op == OP_DISPATCH || h->op == OP_DISPATCH_IND) vk_exec_frame.dispatches++, drew = 1;
    case OP_BIND_PROGRAM: case OP_BIND_STATE: case OP_BIND_VATTR: case OP_BIND_VSTREAM: case OP_BIND_VBUF:
    case OP_BIND_UBO: case OP_BIND_SSBO: case OP_BIND_TEX: case OP_BIND_SAMP: case OP_BIND_IMG: case OP_PUSH_CONST:
        vk_exec_op_draw(ex, h->op, a);
        break;
    case OP_QUEUE: queue_switch(ex, ((const p_queue *)a)->id); break;
    case OP_COPY_B2B: case OP_COPY_B2T: case OP_COPY_T2B: case OP_COPY_T2T: case OP_CLEAR_TEX:
        if (h->op != OP_CLEAR_TEX) vk_exec_frame.copies++;
        if (vk_dump_armed) vk_dump_copy(ex, h->op, a);
        // fall through
    case OP_SIGNAL_EVENT: case OP_REPORT_COUNTER: case OP_RESET_COUNTER: case OP_RENDER_ENABLE: case OP_RENDER_COND:
    case OP_CLEAR_BUF:
        dc_drop(nvn_op_name(h->op));   // a copy or fill may write a layer cleared ahead
        vk_exec_op_xfer(ex, h->op, a);
        break;
    default:
        if (!vk_exec_state_op(ex, h->op, a)) rt_fatal("vk: op %s has no replay handler", nvn_op_name(h->op));
    }
    if (ptm && !ex->in_pass) vk_pt_mark(ex->cb, 0);
    return drew;
}

// MKW_OP_DUMP=FILE (diagnosis, with MKW_TRACE=draw): per draw or dispatch, a line "submission op-position kind ticks"
// (kind: the op, or OPX_HIT / OPX_CAP for MKW_REUSE), to match draws between two serial replays of one capture.
static FILE *op_dump;
static uint64_t op_sub;   // submissions recorded

static void walk(VkExec *ex, const op_hdr *h, int depth) {
    if (op_timing < 0) {
        op_timing = (rt_trace & RT_TRACE_DRAW) != 0;
        const char *f = cfg_get("MKW_OP_DUMP");
        if (op_timing && f && *f && !(op_dump = fopen(f, "w"))) rt_log("vk_exec: MKW_OP_DUMP: cannot write %s", f);
    }
    for (;;) {
        if (h->size < sizeof *h || (h->size & 7) || h->op >= OP_COUNT_)
            rt_fatal("vk: corrupt command record at %p: op %u size %u", (const void *)h, h->op, h->size);
        const void *a = nvn_op_payload(h);
        uint64_t c0 = op_timing ? plat_cycles() : 0;
        int drew = 0;
        switch (h->op) {
        case OP_END: return;
        case OP_BEGIN: break;
        case OP_JUMP: h = (const op_hdr *)(uintptr_t)((const p_jump *)a)->target; continue;
        case OP_CALL:
            if (depth >= CALL_DEPTH) rt_fatal("vk: command buffer calls nested deeper than %d", CALL_DEPTH);
            walk(ex, nvn_op_first(((const p_jump *)a)->target), depth + 1);
            break;
        default: drew = vk_exec_op(ex, h);
        }
        if (op_timing && h->op != OP_CALL) {
            int k = op_hit ? op_hit : (int)h->op;
            uint64_t dt = plat_cycles() - c0;
            op_tsc[k] += dt, op_cnt[k]++, op_hit = 0;
            if (drew && op_dump) fprintf(op_dump, "%llu %llu %d %llu\n", (unsigned long long)op_sub,
                                         (unsigned long long)x_pos, k, (unsigned long long)dt);
        }
        // After a draw or dispatch only: asked after every op once past split_n, the part's clock check (maybe_split)
        // took 22 % of the race's wall clock (rt-h5: 1.4 million calls of 0.36 us).
        if (drew && ++split_draws >= split_n) {
            uint64_t s0 = op_timing ? plat_cycles() : 0;
            maybe_split(ex);
            if (op_timing) op_tsc[OPX_SPLIT] += plat_cycles() - s0, op_cnt[OPX_SPLIT]++;
        }
        h = nvn_op_next(h);
    }
}

// ---- MKW_REPLAY_PAR chunks (vk_exec_par.c)
// A chunk records on its thread into ex->cb from the state the light walk handed it (ex is the chunk's own copy). Its
// command buffer starts with nothing bound, as after a split; its views are its own; feedback layouts and layers cleared
// ahead do not carry over from the chunk before (each chunk ends with a full barrier, every image back in GENERAL).
void vk_exec_chunk_begin(VkExec *ex, const VkImage *seen_depth, int nseen) {
    ex->recorded = 0, ex->in_pass = 0, ex->dirty = VK_EXEC_DIRTY_ALL, ex->rt_resolved = 0;
    ex->views = NULL, ex->nviews = ex->cap_views = 0;
    ex->barrier_at = ex->last_dispatch = ex->clear_run_at = -1;
    ex->nfbl = 0, ex->fbl_mask = 0, ex->fb_dirty = 0, ex->fb_fs = NULL;
    dc_drop("a chunk"), DC.seen = seen_depth, DC.nseen = nseen;
    x_cur = ex;
    vk_exec_draw_thread();
    vk_exec_draw_new_cb();
}

void vk_exec_chunk_end(VkExec *ex) {
    vk_exec_pass_end(ex);
    if (ex->recorded) vk_exec_barrier(ex);
    views_release(ex);
    free(ex->views);
    ex->views = NULL, ex->cap_views = 0;
    x_cur = x_here_ex;   // the replay thread goes back to X (vk_exec_record), a chunk thread to none
    DC.seen = NULL, DC.nseen = 0, dc_drop("a chunk");
}

// MKW_TRACE=perf: the submissions of one frame in 120, logged at its present: when each began recording after the
// previous present, how long it took and how many draws it held. Whether the GPU gets its work late because the game
// submits the scene at the end of the frame, or because the replay holds it back (rt-s0: the game waits ~20 ms for
// the previous frame's GPU work).
#define SUBLOG_N 48
static struct { uint32_t at_us, dur_us, draws, thread; } sublog[SUBLOG_N];
static int nsublog, sublog_on;
static uint64_t sublog_t0;

void vk_exec_present_note(uint64_t frame) {
    if (!(rt_trace & RT_TRACE_PERF)) return;
    if (sublog_on && nsublog) {
        char buf[1600];
        int k = 0;
        for (int i = 0; i < nsublog && k < (int)sizeof buf - 48; i++)
            k += snprintf(buf + k, sizeof buf - (size_t)k, " %.1f+%.1f/%u%s", sublog[i].at_us / 1e3, sublog[i].dur_us / 1e3,
                          sublog[i].draws, sublog[i].thread == sublog[0].thread ? "" : "*");
        rt_log("perf: frame %llu submissions (ms after the previous present + ms recording / draws; * another thread):%s",
               (unsigned long long)frame, buf);
    }
    nsublog = 0;
    sublog_on = frame % 120 == 0;
    sublog_t0 = kern_now_ns();
}

int vk_exec_record(VkCommandBuffer cb, const uint64_t *handles, int n, uint64_t fence) {
    uint64_t sl0 = sublog_on ? kern_now_ns() : 0;
    uint64_t sld = vk_exec_frame.draws_seen;
    VkExec *ex = &X;
    if (!x_ready) state_reset(ex), vk_dump_init(), vk_rtstats_init(), x_ready = 1;
    vk_exec_draw_thread();
    ex->cb = cb;
    ex->fence = fence;
    ex->recorded = 0;
    ex->in_pass = 0;
    ex->dirty = VK_EXEC_DIRTY_ALL;
    ex->rt_resolved = 0;   // images may have changed since the last command buffer
    ex->nviews = 0;
    ex->barrier_at = ex->last_dispatch = ex->clear_run_at = -1;
    dc_drop("a new submission");   // layers cleared ahead belong to the submission that cleared them
    ex->nfbl = 0;   // the previous command buffer ended with a full barrier: every image is in GENERAL
    vk_exec_frame.submits++, op_sub++;
    split_draws = part_start = 0;
    part_t0 = kern_now_ns();
    x_cur = x_here_ex = ex;
    x_pos = 0;
    QS.n = 0, QS.cur = 0;   // queue states live within one submission (queue_switch)
    uint64_t b0 = op_timing > 0 ? plat_cycles() : 0;
    if (n && rec_diff_on > 0) rec_diff_begin();
    if (n) ru_submission(ex);
    if (n) vk_mem_mirror_begin(), vk_usnap_begin(handles, n, fence);
    if (pt_on < 0) pt_on = vk_pt_on();
    if (pt_on > 0 && n) vk_pt_submit_begin(ex->cb);
    if (op_timing > 0) op_tsc[OPX_BEGIN] += plat_cycles() - b0, op_cnt[OPX_BEGIN]++;
    for (int i = 0; i < n; i++)
        if (!handles[i]) rt_fatal("vk: command handle %d of a submission is null", i);
    int par = n ? vk_exec_par(ex, handles, n, op_timing > 0 || pt_on > 0) : 0;
    if (par == 1) part_start = ex->recorded;   // chunks submitted
    else if (!par)
        for (int i = 0; i < n; i++) walk(ex, nvn_op_first(handles[i]), 0);
    b0 = op_timing > 0 ? plat_cycles() : 0;
    vk_exec_pass_end(ex);
    x_cur = x_here_ex = NULL;
    if (ex->recorded) vk_exec_barrier(ex);   // later submissions see this one's writes
    if (pt_on > 0 && n) vk_pt_submit_end(ex->cb, fence), ex->recorded++;
    if (n) ex->recorded += vk_mem_mirror_end(ex->cb, fence), vk_usnap_end(fence);   // VRAM mirror bytes the GPU wrote go back to guest memory
    if (n && rec_diff_on > 0) rec_diff_end();
    views_release(ex);
    ex->cb = VK_NULL_HANDLE;
    if (op_timing > 0) op_tsc[OPX_END] += plat_cycles() - b0, op_cnt[OPX_END]++;
    if (sl0 && nsublog < SUBLOG_N)
        sublog[nsublog++] = (typeof(sublog[0])){(uint32_t)((sl0 - sublog_t0) / 1000), (uint32_t)((kern_now_ns() - sl0) / 1000),
                                                (uint32_t)(vk_exec_frame.draws_seen - sld), plat_thread_id()};
    if (n) n_subs++;
    if ((rt_trace & RT_TRACE_PERF) && n_subs >= 240 && dc_mode > 0) {
        uint64_t rd, sk, ah, bl;
        vk_exec_dc_stats(&rd, &sk, &ah, &bl);
        rt_log("perf: layered depth clears (MKW_DEPTH_CLEAR_ALL): %llu clears of every layer, %llu skipped, %llu "
               "samplings of a layer cleared ahead, %llu layer by layer in a chunk", (unsigned long long)ah,
               (unsigned long long)sk, (unsigned long long)rd, (unsigned long long)bl);
    }
    if ((rt_trace & RT_TRACE_PERF) && n_subs >= 240) {
        rt_log("perf: replay command buffer parts per submission %.2f (%llu splits inside a pass)",
               (double)(n_subs + n_parts) / (double)n_subs, (unsigned long long)n_in_pass);
        n_subs = n_parts = n_in_pass = 0;
        vk_exec_op_timing_log();
        vk_exec_par_log();
        vk_exec_draw_cache_log();
        ru_log();
    }
    return ex->recorded - part_start;   // what the last part holds
}
