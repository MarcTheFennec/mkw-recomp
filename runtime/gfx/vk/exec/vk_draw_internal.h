// Internal interface of the draw replay, split by responsibility: vk_exec_draw.c (program to shaders, feedback loops,
// prepare_draw), vk_exec_cb.c (the command buffer's transient resources and the state recorded into it outside
// descriptors), vk_exec_view.c (stand-ins and image views), vk_exec_ubuf.c (c0 and what uniform blocks hold),
// vk_exec_bind.c (descriptor sets), vk_exec_ops.c (the draw, dispatch and bind ops) and vk_exec_dstat.c (diagnostics).
// What they share: the recording thread's state and their entry points. Everything else goes through vk_exec.h.
// Functions called on every draw stay in the file of their caller, or inline here: across files they would no longer be
// inlined (objdump -dr of the hot functions was compared before and after the split).
#pragma once
#include <stdint.h>
#include <string.h>

#include "cfg.h"
#include "nvn_api.h"
#include "vk.h"
#include "vk_exec.h"

// Data the parts share is hidden: accessed directly (TLS: local-exec), as when it was all one file.
#ifdef __ELF__
#define DRAW_SHARED __attribute__((visibility("hidden")))
#else
#define DRAW_SHARED
#endif

#define ZERO_BYTES 65536u
#define SUPPORT_BYTES 1408u   // Ryujinx SupportBuffer (std140): alpha_test, is_bgra[8], viewport_inverse, viewport_size, ...
#define NVN_STAGE_VS 0
#define NVN_STAGE_FS 1
#define NVN_STAGE_CS 5

enum { W_PROGRAM, W_SHADER, W_PIPELINE, W_NO_VS, W_INDEX, W_UB, W_BIND, W_SSBO_ALIGN, W_SSBO_SLOT, W_RESTART, W_INDIRECT, W_LISTED_R32, W_COUNT };
void warn_once(int key, const char *fmt, ...);   // vk_exec_draw.c

// ---- the recording thread's state (vk_exec_draw.c)
// State of the command buffer being recorded that the Vulkan command buffer itself holds.
typedef struct {
    VkPipeline pipe[2];                  // bound per bind point: 0 graphics, 1 compute
    VkPipeState st;                      // the state vk_pipeline_set_dynamic last recorded
    int st_valid;                        // st is what the bound pipeline was given (cleared at each graphics bind)
    int dyn_valid;                       // st is still what the dynamic state was set from (kept across binds: every
                                         // graphics pipeline lists the same dynamic states; MKW_DYN_DIFF)
    VkBuffer vb[17];                     // vertex buffers last bound
    VkDeviceSize vo[17], vsz[17], vstr[17];
    int vcls[17];                        // their pool class and pool record, for the census
    void *vpool[17];
    NVNbufferRange vin[16];              // the ranges they were resolved from, under vk_mem_gen vgen
    uint32_t vin_used;                   // the streams of vin that were read (the others were bound to zeros)
    uint64_t vgen;
    int vb_valid;
    uint8_t support[SUPPORT_BYTES];      // last support buffer and where it lives
    float sup_in[3];                     // the support_fill arguments it was filled from
    VkBuffer sup_buf;
    VkDeviceSize sup_off;
    int sup_valid;
    uint8_t c0[3][VK_C0_BYTES];          // last c0 bank per stage (vertex, fragment, compute) and where it lives
    VkBuffer c0_buf[3];
    VkDeviceSize c0_off[3];
    int c0_valid[3];
    struct { p_bind_buf ssbo[16], ubo[16]; uint32_t dim[3], binst; uint8_t table[5]; uint64_t tex[128], img[32]; uint8_t pc[NVN_C24_BYTES]; } c0_in[3];   // what vk_c0_fill read for c0[k]
    uint8_t c24[3][NVN_C24_BYTES];       // last push-constant bank c24 per stage and where it lives (c24_info)
    VkBuffer c24_buf[3];
    VkDeviceSize c24_off[3];
    int c24_valid[3];
    VkPipelineLayout ds_layout;          // graphics layout the sets below were last pushed or bound with
    VkDescriptorSet ds_bound[2];         // allocated set bound per set number (0: none or pushed since)
    VkDescriptorSet ds_last[2];          // last allocated and written set per set number, contents in DS
    VkDescriptorSetLayout ds_last_layout[2];
    int ds_pushed[2];                    // set number holds the push of DS.psig under ds_layout
    int res_valid;                       // the graphics sets bound hold what the inputs in RS give (bind_resources)
} DrawB;
extern _Thread_local DrawB *dtl_b DRAW_SHARED;   // vk_exec_draw.c
#define B (*dtl_b)

// The draw or dispatch being bound (vk_exec_bind.c), set by the ops: its dispatch size and base instance, its compute
// shader, and a generation every bind op that writes ex->prog, ubo, ssbo, tex or img bumps.
extern _Thread_local uint32_t c0_dim[3] DRAW_SHARED, c0_binst DRAW_SHARED;
extern _Thread_local const GfxShader *c0_cs DRAW_SHARED;
extern _Thread_local uint64_t bind_gen DRAW_SHARED;
extern _Thread_local int pd_on DRAW_SHARED;            // MKW_TRACE=draw timing (prepare_draw_ex sets it per draw)

// MKW_BIND_STATS counters (vk_exec_bind.c bind_stats).
// MKW_BIND_STATS counters (bind_stats).
typedef struct {
    int on;
    uint64_t draws, bind[2], desc[2], push, push_skip, set_new, set_reuse, same_prog, same_all, bind_skip, ubo_skip;
    uint64_t miss[8];   // graphics builds not skipped, by the first input of bind_sig that differed (bind_stats)
} DrawBS;
extern _Thread_local DrawBS BS DRAW_SHARED;

static inline int bind_stats_on(void) {
    if (BS.on < 0) {
        const char *e = cfg_get("MKW_BIND_STATS");
        BS.on = e && *e && strcmp(e, "0");
    }
    return BS.on;
}

// MKW_TRACE=draw: prepare_draw_ex (PD) and bind_resources (BR) by stage, TSC ticks; vk_exec_dstat.c reports them.
enum { PD_HEAD, PD_SHADER, PD_FEEDBACK, PD_PIPE, PD_STATE, PD_RES, PD_VB, PD_N };
enum { BR_SIG, BR_C0, BR_UBO, BR_SSBO, BR_TEX, BR_CHECK, BR_SETS, BR_N };   // BR_C0: c0, compiler constants, support
extern _Thread_local uint64_t pd_tsc[PD_N] DRAW_SHARED, pd_n DRAW_SHARED, br_tsc[BR_N] DRAW_SHARED;
extern _Thread_local uint64_t td_stat[7] DRAW_SHARED;   // MKW_TRACE=perf: memo hits, of them first in the submission, misses, not memoised; miss TSC, hit TSC, texture lock TSC

// The inline uniform updates of the submission being replayed, newest last (a ring of UPD_N). op_update_ub writes them
// into the buffer's GPU copy; guest memory gets them only from the write-back when the submission ends. The terrain
// rewrites its pointer block this way before every draw, all at one address, so ubo_ptr reads the latest update that
// covers the pointer, else memory. Payloads stay in the recorded command buffer for the whole replay.
#define UPD_N 4096
typedef struct {
    uint64_t fence;                      // the submission the entries belong to
    uint32_t n;                          // updates noted in it
    struct { uint64_t lo, hi; const uint8_t *data; } u[UPD_N];
} DrawUPD;
extern _Thread_local DrawUPD *dtl_upd DRAW_SHARED;   // vk_exec_ubuf.c
#define UPD (*dtl_upd)

static inline void upd_note(const VkExec *ex, const p_update_ub *u) {
    if (UPD.fence != ex->fence) UPD.fence = ex->fence, UPD.n = 0;
    UPD.u[UPD.n++ % UPD_N] = (typeof(UPD.u[0])){u->dst, u->dst + u->size, u->data};
}

// ---- stand-ins for what a binding names but the game did not bind (vk_exec_view.c)
enum { DV_1D, DV_1D_ARRAY, DV_2D, DV_2D_ARRAY, DV_3D, DV_CUBE, DV_CUBE_ARRAY, DV_2D_MS, DV_2D_MS_ARRAY, DV_COUNT };

typedef struct {
    int ready;
    VkBuffer zbuf;
    VmaAllocation zalloc;
    VkBufferView zview;
    VkSampler samp, samp_shadow;
    struct { VkImage image; VmaAllocation alloc; VkImageView view, sview; } img[DV_COUNT];
} DrawZ;
extern DrawZ Z DRAW_SHARED;

// vk_texture_slot_token of the slot a texture or image handle names; an empty handle always resolves to nothing.
static inline uint64_t token_of(uint64_t h) { return h ? vk_texture_slot_token((int)(h & 0xfffff)) : 1; }

// Whether what handle h resolved to under the global generation *egen and its slot token etok (both read before that
// lookup) still holds at global generation gen: nothing at all changed since, or not its slot (the token, read only when
// the global generation moved, about every 16 draws). A token hit moves *egen to gen.
static inline int slot_still(uint32_t *egen, uint64_t etok, uint64_t h, uint32_t gen) {
    if (*egen == gen) return 1;
    uint64_t t = token_of(h);
    if (!t || t != etok) return 0;
    *egen = gen;
    return 1;
}

static inline int in_fbl(const VkExec *ex, VkImage img) {
    for (int i = 0; i < ex->nfbl; i++)
        if (ex->fbl_img[i] == img) return 1;
    return 0;
}

// MKW_TEX_MEMO (tex_info, feedback_scan): 0 off, 1 on, 2 (=check) computes every hit again and stops on a difference.
static inline int td_mode(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = cfg_value("MKW_TEX_MEMO");
        on = !strcmp(e, "check") ? 2 : strcmp(e, "0") != 0;
    }
    return on;
}

enum { VK_C0_DISPATCH = 0x4 };   // compute: the dispatch's group counts in c0 (vk_c0_fill)

// The storage-buffer table of an NVN stage's c0: vertex 0x110, fragment 0x510, compute 0x310 (NVN1's; NVN2 compute uses
// its packed table, gfx_shader.c cs_res), task 0x670 and mesh 0x770 (MKW's task programs read 0x670-0x680 and its mesh
// programs 0x770-0x7b0, the game's slots 0 and up). With 0x610/0x710 they got slots 6 and up, which the game does not
// bind: every task and mesh draw (bushes and grass) read zeros. nxnative docs/reference/shaders.md «Task and mesh draws
// reading the dummy buffer», dmsh_abi.h DMSH_C0_SSBO_TS/MS @ 2a6f9b3 (MIT), issue #1186. -1: none.
static inline int c0_ssbo_base(int stage) {
    switch (stage) {
    case NVN_STAGE_VS: return 0x110;
    case NVN_STAGE_FS: return 0x510;
    case NVN_STAGE_CS: return 0x310;
    case 6: return 0x670;
    case 7: return 0x770;
    default: return -1;
    }
}

// ---- entry points of the parts
// vk_exec_cb.c: the command buffer's resources (made on its first use) and the pipeline bound per bind point
typedef struct CbRes CbRes;
extern _Thread_local CbRes *cur DRAW_SHARED;
extern _Thread_local uint64_t cur_fence DRAW_SHARED;
CbRes *cb_res_new(VkExec *ex);

static inline CbRes *cb_res(VkExec *ex) { return cur && cur_fence == ex->fence ? cur : cb_res_new(ex); }

static inline void bind_pipeline(VkExec *ex, VkPipelineBindPoint bp, VkPipeline p) {
    int k = bp == VK_PIPELINE_BIND_POINT_COMPUTE;
    if (B.pipe[k] == p) {
        if (vk_guard_on() && vk_guard_test() == GUARD_TEST_BIND) vk_guard_test_bind(ex->cb, k);
        if (vk_guard_on()) vk_guard_pipe_hit(ex->cb, k, p);
        return;
    }
    vkCmdBindPipeline(ex->cb, bp, p);
    ex->recorded++;
    B.pipe[k] = p;
    if (!k) B.st_valid = 0;
}

uint8_t *scratch(VkExec *ex, uint64_t size, VkBuffer *buf, VkDeviceSize *off);
void note_written(VkExec *ex, uint64_t addr, uint64_t size);
VkDescriptorSet alloc_set(VkExec *ex, VkDescriptorSetLayout layout);
void record_fixed(VkExec *ex);
void bind_vertex_buffers(VkExec *ex, uint32_t streams);
uint32_t vk_exec_streams(const VkExec *ex, const VkPipeInfo *info);   // the vertex streams info's attributes read
void feedback_barrier(VkExec *ex);
void feedback_written(VkExec *ex, const VkPipeState *s);
void vk_exec_cb_shutdown(void);
// vk_exec_view.c
void dummies_init(void);
void tex_info_full(VkExec *ex, const GfxShaderRes *r, uint64_t h, int storage, int separate, VkDescriptorImageInfo *out,
                   VkImage *image);
// GFX_TYPE_* of the view TEX/IMG resource r gets for handle h (FLOAT when nothing is bound): shader_for's answers
int vk_exec_res_numeric(uint64_t h, const GfxShaderRes *r, int storage);
// vk_exec_draw.c
int find_stage(const prog_t *p, int nvn_stage, const uint8_t **ctl, const uint8_t **code, uint32_t *len);
const GfxShader *shader_for(VkExec *ex, int st, int *present);
void spec_census(VkExec *ex, const GfxShader *s, int st);
int prepare_draw_ex(VkExec *ex, int prim, int32_t count, int restart, int mesh);
// vk_exec_ubuf.c
uint64_t ubo_ptr(const VkExec *ex, int st, const GfxShaderRes *r);
// MKW_BIND_XF (vk_exec_xbind.c, a measurement): descriptors kept across frames by the draw's inputs
int xb_mode(void);
const VkDescSlot *xb_get(uint64_t key, const VkPipeInfo *info, uint32_t nd);
void xb_put(uint64_t key, const VkPipeInfo *info, const VkDescSlot *d, uint32_t nd, const uint8_t *keep,
            const VkDescSlot *had);
typedef struct { uint64_t ubo_max, ubo_align, sbo_max, sbo_align; } DrawLim;
extern DrawLim LIM DRAW_SHARED;   // vk_exec_bind.c
void limits_init(void);
// vk_exec_bind.c
void bind_resources(VkExec *ex, const VkPipeInfo *info);
void vk_exec_bind_thread(void);
void vk_exec_bind_shutdown(void);
// vk_exec_dstat.c
// The last programs drawn or dispatched with, logged when the device is lost (vk_exec_recent_log): a GPU hang names no
// shader (run52/run53: ring timeouts with dmesg and the device coredump closed to the user).
#define RECENT_N 64
typedef struct { uint8_t key[2][32]; uint8_t has_fs; } DrawRecent;
extern DrawRecent recent[RECENT_N] DRAW_SHARED;
extern volatile uint32_t recent_at DRAW_SHARED;

static inline void recent_note(const GfxShader *a, const GfxShader *b) {
    uint32_t i = __atomic_load_n(&recent_at, __ATOMIC_RELAXED);
    const uint8_t *ka = gfx_shader_key(a);
    if (i && !memcmp(recent[(i - 1) % RECENT_N].key[0], ka, 32)) return;   // the same program as the last one
    memcpy(recent[i % RECENT_N].key[0], ka, 32);
    recent[i % RECENT_N].has_fs = b != NULL;
    if (b) memcpy(recent[i % RECENT_N].key[1], gfx_shader_key(b), 32);
    __atomic_store_n(&recent_at, i + 1, __ATOMIC_RELEASE);
}

void repeat_stats(const VkExec *ex, const VkPipeInfo *info);
void ind_trace(VkExec *ex, const p_draw_idx_ind *d, const VkMemSpan *ip, const VkMemSpan *cp, uint32_t max, uint32_t stride);

// The task (if any) and mesh programs of a mesh draw: returns the first stage, *msh the mesh program behind a task
// program (NULL without one); NULL when either translation is missing. *present: the program has a mesh stage.
static inline const GfxShader *mesh_stages(VkExec *ex, const GfxShader **msh, int *present) {
    int has_t;
    const GfxShader *t = shader_for(ex, 6, &has_t);
    const GfxShader *m = shader_for(ex, 7, present);
    *msh = has_t ? m : NULL;
    if (has_t && (!t || !m)) return NULL;
    return has_t ? t : m;
}
