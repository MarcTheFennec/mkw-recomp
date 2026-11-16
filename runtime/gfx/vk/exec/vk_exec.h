// The op-stream replay of the Vulkan back end. vk_exec.c walks the command
// handles of a submission, keeps render targets, passes, clears and fixed-function state; draws, binds and dispatches
// (vk_exec_ops.c and the parts vk_draw_internal.h lists) and copies, counters, events and conditional rendering (vk_exec_xfer.c) plug in through the two
// hooks at the bottom.
// Runs on the submitting thread under vk_sync_submit's lock: no locking inside.
// The op loop follows MacNativeGames nvn_mtl.m (MIT, Copyright (c) 2026 jhaizhou) and the pass handling LAS Recomp
// (MIT, Copyright (c) 2026 Peiikai); vk_exec.c names the sources.
#pragma once
#include <stdint.h>

#include "nvn_api.h"
#include "vk.h"

// Shader stages the game binds resources to: NVN's six and NVN2's 6 and 7 (run98: handle binds to both; by
// hypothesis task and mesh).
#define NVN_BIND_STAGES 8

// Dynamic state the next draw must record (VkExec.dirty). vk_exec_record sets them all at the start of each command
// buffer; the state ops set theirs; vk_exec_cb.c record_fixed clears what it recorded.
enum {
    VK_EXEC_DIRTY_VIEWPORT = 1 << 0,     // viewports or depth range
    VK_EXEC_DIRTY_SCISSOR = 1 << 1,
    VK_EXEC_DIRTY_DEPTH_BIAS = 1 << 2,   // polygon offset
    VK_EXEC_DIRTY_BLEND_CONST = 1 << 3,
    VK_EXEC_DIRTY_STENCIL = 1 << 4,      // compare and write masks, references
    VK_EXEC_DIRTY_ALL = (1 << 5) - 1,
};

// One render target of OP_SET_RT, resolved to its vk_texture.c image for the current command buffer.
typedef struct {
    const tex_t *tex;                    // payload copy (lives for the run); NULL: no target in this slot
    VkImage image;                       // VK_NULL_HANDLE: no target or not renderable (logged once)
    VkFormat format;                     // view format: the view's when it reinterprets a same-size format
    VkImageViewType view_type;           // 2D, or 2D_ARRAY for more than one layer
    VkImageAspectFlags aspect;
    uint32_t level, base_layer, layers;  // subresource of the view; of a 3D image: depth planes
    uint8_t is3d;                        // a 3D image (colour only), viewed through 2D views of its slices
    uint8_t blocks;                      // a BC image viewed as one texel per block (VkTexImage.block_views)
    uint32_t width, height, samples;     // extent of that level; with blocks, in blocks
    int32_t nvn_format;                  // NVNformat of the view (VkPipeState.color_fmt / depth_fmt)
    const VkTexImage *img;               // the image (fb_usage, fb_learned)
} VkExecTarget;

// The replay state. One instance lives for the run: NVN state is queue state and carries over from one submission to
// the next, as in MacNativeGames' and LAS Recomp's replays. The "per command buffer" fields are reset by vk_exec_record.
typedef struct VkExec {
    // ---- per command buffer
    VkCommandBuffer cb;                  // being recorded
    uint64_t fence;                      // timeline value the submission signals (vk_completion_add)
    int recorded;                        // Vulkan commands recorded so far; every handler adds the ones it records
    int in_pass;                         // inside vkCmdBeginRendering
    uint32_t pass_crumb;                 // the open pass's start mark (diag/vk_crumbs.c), 0 none
    uint32_t dirty;                      // VK_EXEC_DIRTY_*

    // ---- render targets (OP_SET_RT) and the current pass
    p_set_rt rt;                         // last OP_SET_RT payload (a change ends the open pass); rt.n 0, no depth: none
    int rt_resolved;                   // color[] and depth hold this command buffer's images for rt
    VkExecTarget color[8], depth;        // resolved targets
    uint32_t pass_w, pass_h, pass_layers;            // render area and layer count of the open (or next) pass
    int pass_noatt;                                   // the pass has no attachment: its size follows the draws (vk_exec_noatt_dims)
    VkFormat pass_color[8], pass_depth, pass_stencil; // attachment formats of the pass (VkPipelineRenderingCreateInfo);
                                                      // VK_FORMAT_UNDEFINED: none
    uint32_t pass_ncolor, pass_samples;               // colorAttachmentCount and rasterization samples of the pass

    // ---- fixed-function state (vk_exec.c keeps it; vk_exec_cb.c records it before draws)
    float vp[16][6];                     // OP_VIEWPORTS: x, y, w, h, near, far
    int has_vp;                          // a viewport was set; else the render area
    p_scissor scissor;                   // OP_SCISSOR (scissor 0)
    int has_scissor;                     // a scissor was set; else unbounded
    float depth_range[2];                // OP_DEPTH_RANGE: overrides the viewports' near and far
    uint32_t st_vmask[2], st_wmask[2], st_ref[2];   // OP_STENCIL per face (0 front, 1 back): compare mask, write mask, ref
    float point_size;                    // OP_BIND_STATE kind 6: NVN point size for the support buffer; 0 = never set = 1
    p_poly_offset poly_offset;           // OP_POLY_OFFSET: depth bias factor, units, clamp
    p_prim_restart prim_restart;         // OP_PRIM_RESTART: applied to indexed strip and fan draws (op_draw_idx)
    float blend_color[4];                // OP_BLEND_COLOR
    float alpha_ref;                     // OP_ALPHA_REF: copied into the draw's VkPipeState when its alpha test is on
    int64_t copy_stride;                 // OP_COPY_STRIDE: row stride of buffer <-> texture copies (vk_exec_xfer.c)
    const dpool_t *tex_pool, *samp_pool; // OP_SET_TEX_POOL, OP_SET_SAMP_POOL
    // Pipeline inputs. vk_exec.c writes depth_clamp (OP_DEPTH_CLAMP), raster_discard (OP_RASTER_DISCARD) and, at pass
    // begin, color_fmt, depth_fmt and samples; vk_exec_draw.c owns every other field (shaders, prim, y_flip, state
    // objects, vertex input).
    VkPipeState pipe;

    // ---- the draw replay's slots (vk_draw_internal.h) (draws, binds, dispatch): left empty by vk_exec.c
    const prog_t *prog;                  // OP_BIND_PROGRAM
    int32_t prog_stages;
    NVNbufferRange vbuf[16];             // OP_BIND_VBUF
    p_bind_buf ubo[NVN_BIND_STAGES][18], ssbo[NVN_BIND_STAGES][16];   // OP_BIND_UBO / OP_BIND_SSBO per stage and index
    uint64_t tex[NVN_BIND_STAGES][128], samp[NVN_BIND_STAGES][128], img[NVN_BIND_STAGES][32];   // OP_BIND_TEX / OP_BIND_SAMP / OP_BIND_IMG handles per stage
    uint8_t c24[NVN_BIND_STAGES][NVN_C24_BYTES];   // OP_PUSH_CONST: the driver's push-constant bank c24 per stage; never pushed reads zero

    // ---- vk_exec_xfer.c slots (render enable, conditional rendering): left empty by vk_exec.c
    int render_disabled;                 // OP_RENDER_ENABLE false or a failed OP_RENDER_COND: clears and draws skip
    p_render_cond render_cond;           // last OP_RENDER_COND
    int has_render_cond;

    // ---- internal to vk_exec.c
    void *views;                         // image views created for this command buffer (freed at its fence)
    int nviews, cap_views;
    int barrier_at;                      // recorded right after the last full barrier (-1: none yet)
    int last_dispatch;                   // recorded right after the last dispatch (MKW_DISPATCH_NOBARRIER)
    int clear_run_at;                    // recorded right after the last image clear (vk_exec_clear.c clear_barrier)
    int nclr;                            // image clears since the last barrier, in clr
    struct { VkImage img; uint32_t level, layer0, layers; } clr[8];

    // Feedback loops (vk.h VkCtx.feedback, vk_exec_draw.c feedback_prepare). Learned targets are attached in
    // ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT; they stay in it across consecutive passes and go back to GENERAL at the next
    // full barrier that is not a pass begin.
    VkImage fbl_img[9];                  // images in the feedback layout now
    VkImageAspectFlags fbl_aspect[9];
    int nfbl;
    uint32_t fbl_mask;                   // attachments of the open pass in that layout: bit i colour i, bit 8 depth
    uint32_t fb_dirty;                   // attachments of the open pass written by a draw or clear since the pass began
                                         // or the last in-pass barrier
    const void *fb_fs;                   // feedback_scan's memo: fragment shader, texture bind serial, pass serial
    uint32_t fb_serial, fb_pass, fb_sampled;
    uint32_t tex_serial, pass_serial;    // bumped by every texture or image bind and pass begin
} VkExec;

// Begins dynamic rendering with the current targets unless a pass is open (a full memory barrier precedes it). Call
// before a draw or an attachment clear.
void vk_exec_pass_begin(VkExec *ex);
// Ends the open pass (no-op without one) and marks its targets GPU-written. Call before copies and dispatches.
void vk_exec_pass_end(VkExec *ex);
// Full memory barrier (all commands, all memory); ends the open pass first. Copies and dispatches order themselves with
// it. Skipped when nothing was recorded since the last one.
void vk_exec_barrier(VkExec *ex);
void wpage_note(const VkExec *ex, uint64_t addr, uint64_t size, const char *what);   // MKW_TRACE_WPAGE (vk_exec_cb.c)
// vk_texture.c (MKW_TEX_ALIAS): commands that must run at the replay's current position. vk_exec_inline_begin returns
// the command buffer being recorded after a full barrier (an open pass is ended; *reopen says so), or VK_NULL_HANDLE
// when this thread is not recording a replay. vk_exec_inline_end begins the pass again when it was open.
// vk_exec_inline_buffer: a device buffer for transfers, destroyed when the submission completes.
VkCommandBuffer vk_exec_inline_begin(int *reopen);
void vk_exec_inline_end(int reopen);
// vk_exec_draw.c: forget what the command buffer being recorded has bound (pipeline, sets, dynamic state, vertex
// buffers): the replay continues in a new one (vk_sync_split).
void vk_exec_draw_new_cb(void);
void vk_exec_draw_forget(void);   // forgets what is bound (pipeline, state, vertex buffers, sets), keeps transient memos
// vk_exec_draw.c: the calling thread's recording state (made once per thread; vk_exec_op_draw makes it too).
void vk_exec_draw_thread(void);
VkBuffer vk_exec_inline_buffer(VkDeviceSize size);
// Scissor 0 clipped to the render area of the open pass; extent 0: nothing to draw.
VkRect2D vk_exec_scissor(const VkExec *ex);
void vk_exec_noatt_dims(const VkExec *ex, uint32_t *w, uint32_t *h);   // pass size when no attachment is set
// 1: every target of the current pass (any 1: at least one) was rendered in the previous presented frame (per-frame
// targets); 0 otherwise, or when the pass has none.
int vk_exec_targets_recent(VkExec *ex, int any);
// The current pass's targets with their formats, extents and the frames they were last rendered in (diagnostics).
void vk_exec_targets_describe(VkExec *ex, char *buf, size_t cap);

// MKW_TRACE=gfx: replay counters of the current frame, logged and reset at each present (vk_exec_frame_log).
typedef struct {
    uint64_t passes, draws_seen, draws, clears, copies, dispatches, submits;
    uint64_t fb_draws, fb_barriers, fb_restarts, fb_layouts;   // feedback loops: draws sampling a written target of
                                                               // their pass, in-pass barriers, pass restarts, layout
                                                               // transitions
    uint64_t skip[8];   // draws not executed, by reason (VK_SKIP_*): the gap between draws_seen and draws
} VkExecFrame;
enum { VK_SKIP_OFF, VK_SKIP_SCISSOR, VK_SKIP_PROGRAM, VK_SKIP_NO_VS, VK_SKIP_SHADER_MISSING,
       VK_SKIP_PIPE_WAIT, VK_SKIP_PIPE_FAILED, VK_SKIP_MESH };
extern VkExecFrame vk_exec_frame_all;
// The counters this thread adds to: vk_exec_frame_all, or a chunk's own on MKW_REPLAY_PAR's threads (summed into it
// when the chunks join, vk_exec_frame_add).
extern _Thread_local VkExecFrame *vk_exec_frame_cur;
#define vk_exec_frame (*vk_exec_frame_cur)
void vk_exec_frame_add(const VkExecFrame *f);
void vk_exec_frame_written(const tex_t *t);                    // vk_texture_gpu_written: t rendered or copied into
uint64_t vk_exec_recording_fence(void);                        // fence of the submission this thread records, else 0
void vk_exec_frame_log(const tex_t *presented, uint64_t frame);
// MKW_IND_READ (vk_ind_read.c): an indirect draw's arguments noted, copied out at its pass end and logged at the fence
void vk_ind_read_note(VkBuffer buf, VkDeviceSize off, uint32_t stride, uint32_t max, VkBuffer cbuf, VkDeviceSize coff,
                      uint32_t vs, int has_count, int indexed, uint64_t guest, const VkExec *ex);   // guest: 0 when
                                                                                                    // not host-backed
void vk_ind_read_pass_end(VkExec *ex);
void vk_ind_note_pass(void);   // the open pass records an indirect draw (MKW_IND_BARRIER)
extern uint64_t vk_exec_presented;   // the last frame vk_exec_frame_log saw (traces that name a frame)
extern int vk_cond_trace;            // MKW_COND_TRACE lines left (vk_exec_xfer.c); -1 before the first condition

// MKW_DUMP=<frame> (vk_dump.c): between presents frame-1 and frame, every pass's targets and every dispatch's storage
// images are copied out and written as PNGs to MKW_DUMP_DIR (default "dump"), with passes.txt (targets, pipelines,
// textures, samplers) and images.txt (per image value statistics). The hooks run only when vk_dump_armed is set.
extern int vk_dump_armed;
void vk_dump_pass_begin(VkExec *ex);   // after vkCmdBeginRendering: the pass's pipeline statistics query
void vk_dump_pass_close(VkExec *ex);   // before vkCmdEndRendering
int vk_dump_rearm(const char *dir, int ubo, int luts, int log_n);   // MKW_CTL: dump the frame log_n + 1 ahead into dir,
                                                                   // logging the ones before; 0 while one is pending
void vk_dump_init(void);                 // reads the environment; vk_exec_record's first call
void vk_dump_shutdown(void);             // after completion callbacks and rtstats workers stop
void vk_dump_pass_end(VkExec *ex);       // after vkCmdEndRendering
void vk_dump_draw(VkExec *ex, const VkPipeState *s, VkPipeline p, const VkPipeInfo *info);   // after a draw's binds;
                                                                                           // marks the draws MKW_DUMP_DRAWS selects
void vk_dump_draw_after(VkExec *ex);     // after vkCmdDraw*: MKW_DUMP_DRAWS copies after the actual draw
void vk_dump_dispatch(VkExec *ex, const GfxShader *cs, VkPipeline p, const VkPipeInfo *info, const p_dispatch *d);
void vk_trace_3d_writes(VkExec *ex, const GfxShader *cs, const VkPipeInfo *info, const p_dispatch *d);
void vk_dump_copy(VkExec *ex, uint32_t op, const void *payload);   // before a copy or texture clear is replayed
void vk_dump_present(uint64_t frame);    // vk_be_present: waits for the dump frame's copies and closes the logs

// MKW_RTSTATS=<N> (diag/vk_rtstats.c): one frame in N, each pass's targets and each dispatch's storage images are
// copied out and checked on a thread of their own; a NaN, an infinity or a constant target is logged once
// ("rtstats:"). The hooks run only when vk_rtstats_on is set.
enum { VK_RTSTATS_NAN, VK_RTSTATS_INF, VK_RTSTATS_CONST, VK_RTSTATS_RULES };
extern int vk_rtstats_on;
void vk_rtstats_init(void);                                   // reads the environment; vk_exec_record's first call
void vk_rtstats_every(uint32_t n);                            // turns it on, one frame in n (--probe-vkexec)
void vk_rtstats_pass_end(VkExec *ex);                         // after vkCmdEndRendering
void vk_rtstats_dispatch(VkExec *ex, const VkPipeInfo *info); // after a dispatch
void vk_rtstats_flush(void);           // waits for every submission and for the checks of their copies
void vk_rtstats_shutdown(void);        // after completion callbacks stop: joins scan workers
uint64_t vk_rtstats_hits(int rule);    // targets that broke the rule so far, logged or not

// c0, the NVN driver constant bank of one stage (vk_exec_ubuf.c; MKW_FIX_C0=0 binds zeros instead): bank is
// VK_C0_BYTES, stage an NVN stage (0 vertex, 1 fragment, 5 compute), ssbo and ubo the 16 bound slots of that stage.
#define VK_C0_BYTES 0x800u
// cs_table (compute, NULL for NVN1's fixed tables): the program's uniform, storage, texture and image entry counts and
// push constant words (gfx_shader_cs_table), with tex[128] and img[32] the stage's bound handles for its packed table.
void vk_c0_fill(uint8_t bank[VK_C0_BYTES], int stage, const p_bind_buf ssbo[16], const p_bind_buf ubo[16],
                const uint32_t dispatch[3], uint32_t base_instance, const uint8_t *cs_table, const uint64_t *tex,
                const uint64_t *img);

// ---- MKW_REPLAY_PAR (vk_exec_par.c): one submission recorded as chunks on several threads
#define VK_EXEC_CALL_DEPTH 8   // OP_CALL nesting
// vk_exec.c: one op that is not END, BEGIN, JUMP or CALL (1: a draw or dispatch); the fixed-function state ops' effect
// alone (1 when op is one); a chunk's start (its state becomes this thread's, x_cur) and end (pass ended, a full barrier,
// its views freed at its fence, x_cur cleared).
int vk_exec_op(VkExec *ex, const op_hdr *h);
void vk_exec_drew(VkExec *ex);   // after a draw vk_exec_op recorded outside walk: the split check
int vk_exec_state_op(VkExec *ex, uint32_t op, const void *a);
// seen_depth: the depth images earlier chunks bound (nseen -1: unknown), which clear layer by layer (dc_layered)
void vk_exec_chunk_begin(VkExec *ex, const VkImage *seen_depth, int nseen);
void vk_exec_op_pos_set(uint64_t pos);   // a chunk's thread: the position before its first op
void vk_exec_chunk_end(VkExec *ex);
// The game's queues' replay states within one submission (OP_QUEUE, nvn_qorder.c): vk_exec_queue_swap saves ex as
// the state of t->cur and loads that of queue id: 2; a queue first seen goes on from ex: 1; 0 when id is current.
// The serial replay ends the pass and puts a full barrier first (vk_exec.c queue_switch); the chunks' walk swaps its own
// table and cuts there, and a chunk's end is that barrier (vk_exec_par.c).
#define VK_EXEC_QUEUES 4
typedef struct {
    struct { uint64_t id; VkExec st; } q[VK_EXEC_QUEUES];
    int n;
    uint64_t cur;
} VkExecQueues;
int vk_exec_queue_swap(VkExecQueues *t, VkExec *ex, uint64_t id);
// vk_exec_par.c: called by vk_exec_record before it walks; 1 when it recorded and submitted the submission's ops as
// chunks (ex then holds the state after the last op and a new command buffer for the rest); 2 when it recorded them
// serially itself (a serial head that the rest of the submission did not follow in chunks); 0: walk as usual. diag: a
// diagnosis that times ops or passes is on (chunks stay off). vk_exec_par_log: the perf line (MKW_TRACE=perf).
int vk_exec_par(VkExec *ex, const uint64_t *handles, int n, int diag);
void vk_exec_par_log(void);
void vk_exec_draw_probe_log(void);   // MKW_DRAW_CACHE_PROBE (vk_exec_dcache.c)
void vk_exec_draw_cache_log(void);   // MKW_DRAW_CACHE (vk_exec_dcache.c)
uint64_t vk_dcache_epoch(void);      // moves when the draw cache empties: the sets it gave before may be destroyed
void vk_exec_op_timing_log(void);    // MKW_TRACE=draw: the replay's time per op since the last call
VkDescriptorSet vk_dcache_get(VkDescriptorSetLayout sl, const VkDescSlot *d, uint32_t n, uint64_t fence,
                              void (*write)(void *ctx, VkDescriptorSet set), void *ctx);   // vk_exec_dcache.c
void vk_dcache_probe(const VkPipeInfo *info, const VkDescSlot *d, uint32_t nd);           // MKW_DRAW_CACHE_PROBE
// vk_exec_ubuf.c: a chunk's thread sees the submission's inline uniform updates before the chunk (upd[0..n)).
void vk_exec_draw_upd_seed(const VkExec *ex, const void *const *upd, int n);
// vk_exec_xfer.c: conditional rendering decided by the light walk. vk_exec_cond_plan follows a report or a condition
// op in walk order (the reports of the walked submission count as pending): the condition's decision (1 render, 0
// skip), -1 for any other op. vk_exec_cond_use: this thread's chunk takes the decisions d[0..n) in op order.
#define VK_COND_PLAN_REPORTS 256
typedef struct { uint64_t addr[VK_COND_PLAN_REPORTS]; uint8_t unknown[VK_COND_PLAN_REPORTS]; int n; } VkCondPlan;
typedef struct { const void *op; int on; } VkCondPlanned;
int vk_exec_cond_plan(VkCondPlan *cp, uint32_t op, const void *payload);
void vk_exec_cond_use(const VkCondPlanned *d, int n);
void vk_exec_xfer_again(int on);   // vk_exec_xfer.c: this thread records a chunk again (no events, no reports)
// vk_exec_xfer.c: samples-passed counting is on (its queries must stay in replay order: no chunks).
int vk_exec_oq_busy(void);
int vk_exec_oq_state(void);   // occlusion counting (bit 0) and a query open (bit 1): whether a draw begins one (MKW_REUSE)
// vk_usnap.c: uniform snapshot copies this thread dropped so far (MKW_REPLAY_PAR's order check).
uint64_t vk_usnap_thread_drops(void);

// Handler hooks. vk_exec.c defines both WEAK: they stop with rt_fatal naming the op.
// vk_exec_ops.c: OP_DRAW, OP_DRAW_IDX, OP_DISPATCH, OP_BIND_* (program, state, vattr, vstream, vbuf, ubo, ssbo, tex,
// samp, img), OP_UPDATE_UB, OP_DRAW_IDX_IND, OP_DISPATCH_IND. payload: nvn_op_payload of the record.
void vk_exec_op_draw(VkExec *ex, uint32_t op, const void *payload);
// The replay-state effect alone of a bind op (vk_exec_ops.c) or a render-enable op (vk_exec_xfer.c), for the light
// walk (MKW_REPLAY_WALK): 1 when op is one of theirs.
int vk_exec_bind_state(VkExec *ex, uint32_t op, const void *payload);
int vk_exec_xfer_state(VkExec *ex, uint32_t op, const void *payload);
void vk_exec_dc_read(VkImage img);           // a sampling of img (MKW_DEPTH_CLEAR_ALL's read count)
// since the last call; by_layer: clears a chunk made layer by layer since an earlier chunk bound the image
void vk_exec_dc_stats(uint64_t *reads, uint64_t *skipped, uint64_t *ahead, uint64_t *by_layer);
void vk_exec_present_note(uint64_t frame);   // MKW_TRACE=perf: a present (the per-frame submission census)
int vk_exec_draw_timing(char *buf, int size, double us_per_tick);   // MKW_TRACE=perf: prepare_draw by stage, then reset
// The aspect a sampled view of img reads (vk_exec_view.c get_view): stencil for a view in STENCIL depth-stencil mode.
VkImageAspectFlags vk_exec_view_aspect(const VkTexImage *img, const view_t *v, int hv);
VkImageView vk_exec_target_view(const VkTexImage *img, VkFormat format, VkImageViewType type, uint32_t aspect,
                                uint32_t level, uint32_t base, uint32_t layers, int blocks);   // kept for the image's life
// vk_exec_xfer.c: OP_COPY_B2B, OP_COPY_B2T, OP_COPY_T2B, OP_COPY_T2T, OP_CLEAR_TEX, OP_SIGNAL_EVENT, OP_REPORT_COUNTER,
// OP_RESET_COUNTER, OP_RENDER_ENABLE, OP_RENDER_COND, OP_CLEAR_BUF.
void vk_exec_op_xfer(VkExec *ex, uint32_t op, const void *payload);
// vk_exec_xfer.c, samples-passed counting: a draw about to be recorded in the open pass (begins an occlusion query
// when counting and none is active), and the open pass about to end (ends the active query).
void vk_exec_oq_draw(VkExec *ex);
void vk_exec_oq_pass_end(VkExec *ex);
