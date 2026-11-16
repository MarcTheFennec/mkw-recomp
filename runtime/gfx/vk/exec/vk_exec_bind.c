// Descriptor sets of the draw replay (vk_draw_internal.h): per draw or dispatch, the pipeline's sets written from the
// bound state (bind_resources) and pushed, or allocated and bound; buffer ranges, c0, the support buffer and texture
// descriptors with their memos; a graphics draw whose descriptor inputs equal the previous one's skips the build.
// Everything bind_resources calls on every draw is in this file, where it is inlined.
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "nvn_api.h"
#include "nvn_hmap.h"
#include "tex.h"
#include "vk.h"
#include "vk_draw_internal.h"
#include "vk_exec.h"
#include "vk_reuse.h"
#include "platform/platform.h"

#define MAX_BINDINGS 128
#define MAX_DESC 1024

// Contents of B.ds_last: the set's packed descriptors (VkDescSlot), which with its layout determine every write. A draw
// whose allocated set would hold the same reuses the set instead of allocating and writing one (a per-draw
// cost). psig: the same for the last push per set number; a push equal to it under the same layout is not recorded
// again (the descriptors stay bound across pipeline binds of that layout).
#define DS_SIG_BYTES (MAX_DESC * sizeof(VkDescSlot))
typedef struct { uint32_t n[2], pn[2]; uint8_t sig[2][DS_SIG_BYTES], psig[2][DS_SIG_BYTES]; } DrawDS;
static _Thread_local DrawDS *dtl_ds;
#define DS (*dtl_ds)

_Thread_local DrawBS BS = {-1};

// The inputs of the last graphics bind_resources (bind_sig) and what its build counted for the traffic census, which a
// skipped build counts again. RS_BYTES bounds the bound-slot bytes a pipeline reads (RsList): per binding at most one
// c0 storage table, otherwise one bound range or handle per descriptor.
#define RS_BYTES (MAX_BINDINGS * sizeof(((VkExec *)0)->ssbo[0]) + MAX_DESC * sizeof(p_bind_buf))
typedef struct { int cls, use; uint64_t bytes; } RsCount;
typedef struct {
    int on;                              // MKW_BIND_SKIP: 1 skip (default), 2 check, 0 off
    uint32_t hn;                         // words of h the last signature filled; 0: none
    int why;                             // MKW_BIND_STATS: the first input that differed (BS.miss)
    uint64_t h[24];                      // generations, feedback images, support inputs, base instance
    const VkPipeInfo *info;              // the pipeline whose RsList ranges body holds, as of bind op generation gen
    uint64_t gen;
    uint8_t body[RS_BYTES];
    int rec;                             // a graphics build is running: count_use records, scratch copies mark volatile
    int volatile_;                       // the build copied guest bytes into transient memory: a rebuild differs
    RsCount cnt[2 * VK_MCLS_N], pcnt[2 * VK_MCLS_N];
    int ncnt, pncnt, cnt_over;
    int cmds;                            // check mode: pushes, binds and set writes the build recorded
    uint32_t ckn;                        // check mode: the last build's packed descriptors (bytes in ck)
    uint8_t ck[DS_SIG_BYTES];
} DrawRS;
static _Thread_local DrawRS *dtl_rs;
#define RS (*dtl_rs)

// The bound-slot bytes of VkExec a pipeline's descriptors read, as ranges merged where they touch, built once per
// pipeline info: c0's storage table, the uniform and storage ranges of the bound slots and the texture and image
// handles. sup and c0: the support inputs and the base instance count too. p: storage buffers whose address the shader
// loads from a user uniform block; the address read from the block counts too. ok 0: never skipped (too many
// descriptors or such buffers).
#define RS_PTRS 4
typedef struct { uint32_t off, len, uniform; } RsRange;   // uniform: a user uniform block's binding (MKW_BIND_XF skips it)
typedef struct { int st; const GfxShaderRes *res; } RsPtr;
typedef struct { uint32_t n, bytes; uint8_t sup, c0, ok, np; RsPtr p[RS_PTRS]; RsRange r[]; } RsList;
static nvn_hmap rs_lists;                // VkPipeInfo pointer -> RsList, until shutdown; rs_lock
static PlatMutex rs_lock = PLAT_MUTEX_INIT;
static _Thread_local const VkPipeInfo *rs_last;        // the last lookup
static _Thread_local const RsList *rs_last_l;

_Thread_local uint32_t c0_dim[3], c0_binst;
_Thread_local const GfxShader *c0_cs;
_Thread_local uint64_t bind_gen = 1;

// ---- descriptors

static int nvn_stage_of(int gfx_stage) {
    if (gfx_stage & VK_PIPE_STAGE_NVN) return gfx_stage & 7;   // a task or mesh binding (vk_pipeline.c add_stage)
    return gfx_stage == GFX_STAGE_VS ? NVN_STAGE_VS : gfx_stage == GFX_STAGE_FS ? NVN_STAGE_FS : NVN_STAGE_CS;
}

static VkDescriptorBufferInfo zero_info(uint64_t max) {
    return (VkDescriptorBufferInfo){Z.zbuf, 0, max < ZERO_BYTES ? max : ZERO_BYTES};
}

// vk_mem_count, also kept per (class, use) while a graphics build runs (RS).
static void count_use(int cls, int use, uint64_t n) {
    vk_mem_count(cls, use, n);
    if (!RS.rec) return;
    for (int i = 0; i < RS.ncnt; i++)
        if (RS.cnt[i].cls == cls && RS.cnt[i].use == use) {
            RS.cnt[i].bytes += n;
            return;
        }
    if (RS.ncnt < (int)(sizeof RS.cnt / sizeof RS.cnt[0])) RS.cnt[RS.ncnt++] = (RsCount){cls, use, n};
    else RS.cnt_over = 1;
}

// A buffer descriptor for [addr, addr+size) of guest memory: its backing (a pool's VRAM mirror when it has one) when
// aligned, a copy in transient memory when the backing is host memory with an unaligned offset. 0 when neither works.
// written: the shader stores into it (a mirror's range goes back to guest memory at the end of the submission).
static int bind_range(VkExec *ex, uint64_t addr, uint64_t size, uint64_t align, uint64_t max, int use, int written,
                      VkDescriptorBufferInfo *out) {
    VkMemSpan sp;
    if (!addr || !vk_mem_resolve(addr, 1, &sp)) return 0;
    uint64_t n = size && size < sp.avail ? size : sp.avail;
    if (n > max) n = max;
    if (!n) return 0;
    if (sp.offset % align == 0) {
        VK_MEM_USE(sp);
        *out = (VkDescriptorBufferInfo){sp.buffer, sp.offset, n};
        if (written) vk_mem_gpu_wrote(&sp, n), vk_usnap_drop(addr, n);
        if (written && ru_cap) ru_note_bad(RUB_WRITTEN);
        count_use(sp.cls, use, n);
        VK_MEM_BOUND(sp);
        return 1;
    }
    if (sp.alloc) return 0;
    VkBuffer b;
    VkDeviceSize o;
    RS.volatile_ = 1;
    if (ru_cap) ru_note_bad(RUB_VOLATILE);   // bytes copied at record time
    memcpy(scratch(ex, n, &b, &o), (const void *)(uintptr_t)addr, n);
    // MKW_TRACE_SCRATCH=1 (diagnosis): each copy at record time, with its replay position and a hash of the bytes: two
    // recording orders of a capture told apart by what such a copy found (the title's order fault, grabacion-paralela).
    static int trace_scratch = -1;
    if (trace_scratch < 0) trace_scratch = cfg_get("MKW_TRACE_SCRATCH") && atoi(cfg_get("MKW_TRACE_SCRATCH"));
    if (trace_scratch) {
        uint64_t h = 0xcbf29ce484222325ull;
        for (uint64_t i = 0; i < n; i++) h = (h ^ ((const uint8_t *)(uintptr_t)addr)[i]) * 0x100000001b3ull;
        rt_log("vk: scratch copy %#llx+%llu use %d at op %llu: %016llx", (unsigned long long)addr, (unsigned long long)n,
               use, (unsigned long long)vk_exec_op_pos(), (unsigned long long)h);
    }
    vk_mem_count(sp.cls, VK_MUSE_CPU_READ, n);
    vk_mem_count(VK_MCLS_SCRATCH, use, n);
    *out = (VkDescriptorBufferInfo){b, o, n};
    return 1;
}

static int c0_on(void) {   // MKW_FIX_C0=0: c0 reads zeros instead of the driver constants
    static int on = -1;
    if (on < 0) {
        on = cfg_bool("MKW_FIX_C0");
    }
    return on;
}

static int c24_on(void);

// c0 from replay scratch; a bank equal to the stage's previous one reuses its range, so it costs no copy.
// An NVN2 compute program reads its pushed constants in c0 from byte 0x10, as many words as its control section says
// (+0xa9, gfx_shader.c cs_res), before its resource table: there they go (OP_PUSH_CONST of stage NVN_STAGE_CS). The
// photo mode's 1.5x scale of the picture to 3840x2160 (cs 07a03741f7478a83) clamps and offsets its reads by them;
// with zeros it wrote black and photo mode showed a black screen (issue #384; ph8, 1440 lines, 16/11/2026).
static VkDescriptorBufferInfo c0_info(VkExec *ex, int st) {
    int k = st == NVN_STAGE_VS ? 0 : st == NVN_STAGE_FS ? 1 : 2;
    // vk_c0_fill reads the storage slots, and the uniform slots and dispatch size for compute or the base instance for
    // graphics: equal inputs give the same bank, so neither the fill nor the 2 KiB compare is needed.
    typeof(B.c0_in[0]) in;
    memset(&in, 0, sizeof in);
    memcpy(in.ssbo, ex->ssbo[st], sizeof in.ssbo);
    const uint8_t *table = NULL;
    if (st == NVN_STAGE_CS) {
        memcpy(in.ubo, ex->ubo[st], sizeof in.ubo), memcpy(in.dim, c0_dim, sizeof in.dim);
        if ((table = gfx_shader_cs_table(c0_cs))) {   // NVN2 packed table: its handles are part of the bank
            memcpy(in.table, table, 5);
            memcpy(in.tex, ex->tex[st], sizeof in.tex), memcpy(in.img, ex->img[st], sizeof in.img);
            if (c24_on()) memcpy(in.pc, ex->c24[st], sizeof in.pc);
        }
    } else {
        in.binst = c0_binst;
    }
    if (B.c0_valid[k] && !memcmp(&in, &B.c0_in[k], sizeof in))
        return (VkDescriptorBufferInfo){B.c0_buf[k], B.c0_off[k], VK_C0_BYTES};
    B.c0_in[k] = in;
    uint8_t tmp[VK_C0_BYTES];
    vk_c0_fill(tmp, st, ex->ssbo[st], ex->ubo[st], c0_dim, c0_binst, table, ex->tex[st], ex->img[st]);
    if (table) {   // the pushed constants, before the table vk_c0_fill wrote past them
        uint32_t n = 4u * table[4];
        if (n > NVN_C24_BYTES) n = NVN_C24_BYTES;
        if (0x10 + n <= VK_C0_BYTES) memcpy(tmp + 0x10, in.pc, n);
    }
    if (!B.c0_valid[k] || memcmp(tmp, B.c0[k], VK_C0_BYTES)) {
        memcpy(scratch(ex, VK_C0_BYTES, &B.c0_buf[k], &B.c0_off[k]), tmp, VK_C0_BYTES);
        memcpy(B.c0[k], tmp, VK_C0_BYTES);
        B.c0_valid[k] = 1;
    }
    return (VkDescriptorBufferInfo){B.c0_buf[k], B.c0_off[k], VK_C0_BYTES};
}

static int c24_on(void) {   // MKW_PUSH_CONST=0: c24 reads zeros, as before the push constants were passed
    static int on = -1;
    if (on < 0) on = rt_knob("PUSH_CONST", "1");
    return RT_KNOB_ON(on);
}

// The push-constant bank c24 (OP_PUSH_CONST) from replay scratch; a bank equal to the stage's previous one reuses its
// range. The shaders declare the bank at its full 64 KiB: reads past these 256 bytes return zero (robustBufferAccess).
static VkDescriptorBufferInfo c24_info(VkExec *ex, int st) {
    int k = st == NVN_STAGE_VS ? 0 : st == NVN_STAGE_FS ? 1 : 2;
    if (!B.c24_valid[k] || memcmp(ex->c24[st], B.c24[k], NVN_C24_BYTES)) {
        memcpy(scratch(ex, NVN_C24_BYTES, &B.c24_buf[k], &B.c24_off[k]), ex->c24[st], NVN_C24_BYTES);
        memcpy(B.c24[k], ex->c24[st], NVN_C24_BYTES);
        B.c24_valid[k] = 1;
    }
    return (VkDescriptorBufferInfo){B.c24_buf[k], B.c24_off[k], NVN_C24_BYTES};
}

// The buffer range limits, read once: the device's properties are a cold cache line on every binding of every draw.
DrawLim LIM;

void limits_init(void) {
    const VkPhysicalDeviceLimits *l = &vk_ctx()->props.limits;
    LIM.ubo_align = l->minUniformBufferOffsetAlignment, LIM.sbo_max = l->maxStorageBufferRange;
    LIM.sbo_align = l->minStorageBufferOffsetAlignment;
    __atomic_store_n(&LIM.ubo_max, l->maxUniformBufferRange, __ATOMIC_RELEASE);
}

static _Thread_local int ubo_kind;   // MKW_REUSE: what gave ubo_info's last answer (RUK_*)

static VkDescriptorBufferInfo ubo_info(VkExec *ex, int st, int slot) {
    ubo_kind = RUK_STABLE;
    if (!__atomic_load_n(&LIM.ubo_max, __ATOMIC_ACQUIRE)) limits_init();
    uint64_t max = LIM.ubo_max, align = LIM.ubo_align;
    VkDescriptorBufferInfo bi;
    if (slot == 0 && c0_on()) return ubo_kind = RUK_C0, c0_info(ex, st);
    if (slot == 1 && ex->prog) {   // compiler constants stored after the shader code
        // NVN2 control: +0x3c their size, +0x40 their offset in the code blob, +0x44 the blob's size (all 11 561
        // programs of 1.0.0, 357 with constants). This read NVN1's control +0x700, past the 0xd8-byte NVN2 control: c1
        // was garbage and the text VS took every glyph corner's u,v from the same entry (01/11/2026).
        const uint8_t *ctl, *code;
        uint32_t len, c1_size, c1, total;
        if (find_stage(ex->prog, st, &ctl, &code, &len)) {
            memcpy(&c1_size, ctl + 0x3c, 4), memcpy(&c1, ctl + 0x40, 4), memcpy(&total, ctl + 0x44, 4);
            if (c1_size && c1 < total && c1_size <= total - c1 &&
                bind_range(ex, (uint64_t)(uintptr_t)code + c1, c1_size, align, max, VK_MUSE_UBO, 0, &bi))
                return bi;
        }
    } else if (slot == 24 && c24_on()) {
        return ubo_kind = RUK_C24, c24_info(ex, st);
    } else if (slot >= 3 && slot - 3 < 18) {
        const p_bind_buf *b = &ex->ubo[st][slot - 3];
        if (b->size && vk_usnap_find(b->addr, b->size < max ? b->size : max, align, &bi)) {   // as of the submit call
            count_use(VK_MCLS_SCRATCH, VK_MUSE_UBO, bi.range);
            ubo_kind = RUK_UBO;
            return bi;
        }
        if (bind_range(ex, b->addr, b->size, align, max, VK_MUSE_UBO, 0, &bi)) return bi;
    }
    return zero_info(max);
}

// The NVN slot of a storage binding. Ryujinx numbers a shader's storage buffers in the order it meets them
// (GfxShaderRes.slot); the NVN slot is the entry of the driver constant buffer c0 that holds the buffer's address
// (cbuf_slot 0, handle = word offset): 16-byte entries at byte 0x110 (vertex), 0x510 (fragment) and 0x310 (compute).
// A compute address at c0 0x210..0x30f is uniform block (offset - 0x210) / 16 read as storage: *ubo is set. -1: the
// address is not in c0 (the shader loads it from a user uniform block: ubo_ptr) or the stage has no known table.
// Mapping: MacNativeGames games/totk/tools/totk_xlate/Service.cs:206-222 @ b75be8cf (MIT, Copyright (c) 2026 jhaizhou).
static int ssbo_slot(const GfxShaderRes *r, int st, int *ubo) {
    *ubo = 0;
    if (r->cbuf_slot != 0) return -1;
    int off = r->handle * 4, base = c0_ssbo_base(st);
    if (st == NVN_STAGE_CS && off >= 0x210 && off < 0x310 && (off - 0x210) % 16 == 0) {
        *ubo = 1;
        return (off - 0x210) / 16;
    }
    return base >= 0 && off >= base && off < base + 0x100 && (off - base) % 16 == 0 ? (off - base) / 16 : -1;
}

// 1 when the shader loads the storage buffer's address from a user uniform block (cbuf_slot 3.., NVN uniform slot
// cbuf_slot - 3, byte offset handle * 4). The Depths terrain reads its vertices through one, about 500 draws a frame;
// with a zero buffer they collapse and the ground is missing. MacNativeGames resolves these the same way (NVN_RB_SBUFU,
// games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m @ b75be8cf).
static int ubo_ptr_res(const GfxShaderRes *r) {
    return r->cbuf_slot >= 3 && r->cbuf_slot - 3 < 18;
}

static VkDescriptorBufferInfo ssbo_info(VkExec *ex, int st, const GfxShaderRes *r, int written) {
    if (!__atomic_load_n(&LIM.ubo_max, __ATOMIC_ACQUIRE)) limits_init();
    uint64_t max = LIM.sbo_max, align = LIM.sbo_align;
    VkDescriptorBufferInfo bi;
    int ubo, slot = ssbo_slot(r, st, &ubo);
    p_bind_buf up = {0};
    const p_bind_buf *b = NULL;
    if (slot >= 0 && slot < 16) {
        b = ubo ? &ex->ubo[st][slot] : &ex->ssbo[st][slot];
        if (ru_cap) ru_note_slot(ubo ? 2 : 1, st, slot);
    }
    else if (ubo_ptr_res(r)) {   // size 0: up to the end of the backing
        up.addr = ubo_ptr(ex, st, r), b = &up;
        if (ru_cap) ru_note_bad(RUB_UBO_PTR);   // read from the block's bytes, which change without a bind
    }
    if (b) {
        // vk_caps.c: shaders translated for sbuf_align address the buffer from its address rounded down to it.
        uint64_t lead = vk_caps.sbuf_align ? b->addr & (vk_caps.sbuf_align - 1) : 0;
        // A buffer the shader only reads is bound to the end of its backing: global loads on the console are not held
        // to the size in c0 (the HUD's text VS reads 64-byte glyph records, one per instance, from a binding of 0x100
        // bytes: past the fourth they read zero).
        uint64_t size = written && b->size ? b->size + lead : 0;
        if (bind_range(ex, b->addr - lead, size, align, max, ubo ? VK_MUSE_UBO : VK_MUSE_SSBO,
                       written, &bi)) {
            if (written && bi.buffer != VK_NULL_HANDLE) note_written(ex, b->addr - lead, bi.range);
            return bi;
        }
        if (b->addr) warn_once(W_SSBO_ALIGN, "storage buffer %#llx is not backed or not aligned: a zero buffer is bound",
                               (unsigned long long)b->addr);
    } else {
        warn_once(W_SSBO_SLOT, "a storage buffer whose address is at c%d+%#x (stage %d) is not replayed: a zero buffer is bound",
                  r->cbuf_slot, r->handle * 4, st);
    }
    return zero_info(max);
}

// ---- textures

// Sampled-texture descriptors per (handle, resource type, separate), kept while nothing they derive from changed: the
// slot's resolution and texture entry (slot_still) and the samplers (vk_sampler_gen); the layout is set again on every
// hit from the images in the feedback layout (in_fbl of the image kept with it). The first binding of a slot in a submission (vk_texture_epoch) still makes the slot lookup
// for its storage check; a re-upload writes the same image, so the descriptor stays. Draws bind the same textures
// thousands of times a frame (15,000 lookups a frame in the monsters fight); a hit skips the slot lookup, the view key
// with its hash probe and the sampler lookup. Storage images, which note their binding per submission, are
// not kept. Direct-mapped, per recording thread; a hit is checked against the generations and the slot token, all read
// atomically, so it takes no lock: only a miss or the submission's first binding of a slot takes the texture lock (the
// chunks of MKW_REPLAY_PAR bind on several threads, and the lock around every draw's textures made them wait on it).
// MKW_TEX_MEMO=0 turns it off; =check computes every hit again and stops on a difference.
#define TD_N 8192
typedef struct {
    uint64_t h, tok;
    uint32_t gen, epoch, sgen;
    uint16_t type;
    uint8_t separate, valid;
    VkImage image;
    VkDescriptorImageInfo out;
} TexMemo;
static _Thread_local TexMemo *TD;   // TD_N per recording thread (vk_exec_draw_thread)

_Thread_local uint64_t td_stat[7];

static void tex_lock_once(int *locked) {
    if (*locked) return;
    uint64_t t = pd_on ? plat_cycles() : 0;
    vk_texture_lock(), *locked = 1;
    if (pd_on) td_stat[6] += plat_cycles() - t;
}

// locked: whether this thread holds the texture lock; taken here when a lookup needs it, released by the caller.
static void tex_info(VkExec *ex, const GfxShaderRes *r, uint64_t h, int storage, int separate, VkDescriptorImageInfo *out,
                     int *locked) {
    int td_on = td_mode();
    VkImage image;
    if (!h || storage || !td_on) {
        tex_lock_once(locked);
        td_stat[3]++, tex_info_full(ex, r, h, storage, separate, out, &image);
        vk_exec_dc_read(image);
        if (ru_cap) ru_note_tex(h, image, storage);
        return;
    }
    uint64_t th = pd_on ? plat_cycles() : 0;
    uint32_t gen = vk_texture_slots_gen(), ep = vk_texture_epoch(), sg = vk_sampler_gen();   // before any lookup
    typeof(TD[0]) *m = &TD[(h * 0x9E3779B97F4A7C15ull ^ (uint64_t)r->type << 7 ^ (uint64_t)separate) >> 51];
    if (m->valid && m->h == h && m->type == r->type && m->separate == separate && m->sgen == sg &&
        slot_still(&m->gen, m->tok, h, gen)) {
        td_stat[0]++;
        if (m->epoch != ep) {   // the slot's first binding in this submission: only its storage check is due
            // unless another thread (a chunk of MKW_REPLAY_PAR, lpc1) already made it in this submission;
            // MKW_TEX_CHECKED=0 takes the lock every time, to measure
            static int chk = -1;
            if (chk < 0) chk = !cfg_get("MKW_TEX_CHECKED") || strcmp(cfg_get("MKW_TEX_CHECKED"), "0") != 0;
            if (!chk || !vk_texture_checked((int)(h & 0xfffff), ep)) {
                tex_lock_once(locked);
                vk_texture_by_id_locked((int)(h & 0xfffff), NULL, NULL);
            }
            m->epoch = ep;
            td_stat[1]++;
        }
        *out = m->out;
        vk_exec_dc_read(m->image);
        if (ru_cap) ru_note_tex(h, m->image, 0);
        out->imageLayout = m->image && ex->nfbl && in_fbl(ex, m->image) ? VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT
                                                                          : VK_IMAGE_LAYOUT_GENERAL;
        if (td_on == 2) {
            VkDescriptorImageInfo full;
            tex_lock_once(locked);
            tex_info_full(ex, r, h, storage, separate, &full, &image);
            if (full.sampler != out->sampler || full.imageView != out->imageView || full.imageLayout != out->imageLayout)
                rt_fatal("vk_exec: texture descriptor memo of handle %#llx type %#x differs from a lookup",
                         (unsigned long long)h, r->type);
        }
        if (pd_on) td_stat[5] += plat_cycles() - th;
        return;
    }
    uint64_t tok = token_of(h);   // before the lookup
    td_stat[2]++;
    uint64_t t0 = pd_on ? plat_cycles() : 0;
    tex_lock_once(locked);
    tex_info_full(ex, r, h, storage, separate, out, &image);
    vk_exec_dc_read(image);
    if (ru_cap) ru_note_tex(h, image, 0);
    if (pd_on) td_stat[4] += plat_cycles() - t0;
    *m = (typeof(*m)){.h = h, .tok = tok, .gen = gen, .epoch = ep, .sgen = sg, .type = r->type,
                      .separate = (uint8_t)separate, .valid = 1, .image = image, .out = *out};
}

static void support_in(const VkExec *ex, float in[3]) {   // what support_fill reads: viewport size, point size
    in[0] = ex->in_pass && ex->has_vp ? ex->vp[0][2] : (float)ex->pass_w;
    in[1] = ex->in_pass && ex->has_vp ? ex->vp[0][3] : (float)ex->pass_h;
    in[2] = ex->point_size > 0 ? ex->point_size : 1.0f;
}

static void support_fill(uint8_t *b, float w, float h, float point_size);

// The support buffer for the bound viewport and point size: the last one of this command buffer when its inputs or
// contents are the same, else a new one in scratch.
static VkDescriptorBufferInfo support_info(VkExec *ex) {
    float in[3];
    support_in(ex, in);
    if (!B.sup_valid || memcmp(in, B.sup_in, sizeof in)) {   // support_fill reads only these
        uint8_t tmp[SUPPORT_BYTES];
        support_fill(tmp, in[0], in[1], in[2]);
        memcpy(B.sup_in, in, sizeof in);
        if (!B.sup_valid || memcmp(tmp, B.support, SUPPORT_BYTES)) {   // a new buffer only when the contents changed
            VkBuffer sb;
            VkDeviceSize so;
            memcpy(scratch(ex, SUPPORT_BYTES, &sb, &so), tmp, SUPPORT_BYTES);
            memcpy(B.support, tmp, SUPPORT_BYTES);
            B.sup_buf = sb, B.sup_off = so, B.sup_valid = 1;
        }
    }
    return (VkDescriptorBufferInfo){B.sup_buf, B.sup_off, SUPPORT_BYTES};
}

static void support_fill(uint8_t *b, float w, float h, float point_size) {
    memset(b, 0, SUPPORT_BYTES);
    float inv[4] = {w > 0 ? 2.0f / w : 0, h > 0 ? 2.0f / h : 0, 0, 0}, size[4] = {w, h, 0, 0};
    memcpy(b + 144, inv, 16);
    memcpy(b + 160, size, 16);
    const float one = 1.0f;
    memcpy(b + 1392, &point_size, 4);   // point_size (translator patch 0008): vertex shaders that write none output it
    for (int i = 0; i < 73; i++) memcpy(b + 192 + 16 * i, &one, 4);   // render scale 1: the host renders at guest size
}

// MKW_BIND_STATS=1: bindings and descriptors per draw of each stage (Task 2.2's choice of the pushed stage), and how
// often a draw repeats the previous one, logged every 4000 draws (a capture holds ~12000): pushes recorded and skipped,
// allocated sets written
// and reused, draws with the previous draw's program, bound state
// and textures (shaders and pipeline would be the same) and those that also have its buffers and viewport (all
// descriptors would be the same). BS and bind_stats_on are in vk_draw_internal.h.
static void bind_stats(const VkPipeInfo *info) {
    if (!bind_stats_on() || info->bind_point != VK_PIPELINE_BIND_POINT_GRAPHICS) return;
    for (uint32_t i = 0; i < info->n_bindings; i++) {
        const VkPipeBinding *b = &info->bindings[i];
        int fs = info->n_sets > 1 ? b->set : !(b->stages & VK_SHADER_STAGE_VERTEX_BIT);
        BS.bind[fs]++, BS.desc[fs] += b->count ? b->count : 1;
    }
    if (++BS.draws % 4000 == 0) {
        double d = (double)BS.draws;
        rt_log("vk_exec: per draw over %llu draws: vertex %.2f bindings %.2f descriptors, fragment %.2f bindings %.2f descriptors",
               (unsigned long long)BS.draws, BS.bind[0] / d, BS.desc[0] / d, BS.bind[1] / d, BS.desc[1] / d);
        rt_log("vk_exec: per draw: pushes %.3f skipped %.3f, sets written %.3f reused %.3f; "
               "same program and state as the draw before %.3f, also same buffers %.3f; descriptor builds skipped %.3f",
               BS.push / d, BS.push_skip / d, BS.set_new / d, BS.set_reuse / d, BS.same_prog / d,
               BS.same_all / d, BS.bind_skip / d);
        rt_log("vk_exec: per draw: rebinds with new uniform block offsets only %.3f (MKW_UBO_SKIP)", BS.ubo_skip / d);
        rt_log("vk_exec: per draw, builds not skipped by the first input that differed: pipeline %.3f, program %.3f, "
               "texture slots or epoch %.3f, samplers or feedback count %.3f, memory or uniform snapshot generation %.3f, "
               "feedback images, support, base instance or uniform-read addresses %.3f, bound slots %.3f, only user "
               "uniform blocks' bindings %.3f",
               BS.miss[0] / d, BS.miss[1] / d, BS.miss[2] / d, BS.miss[3] / d, BS.miss[4] / d, BS.miss[5] / d,
               BS.miss[6] / d, BS.miss[7] / d);
    }
}

static int push_skip_on(void) {   // MKW_PUSH_SKIP=0: every graphics draw pushes its set again
    static int on = -1;
    if (on < 0) {
        on = cfg_bool("MKW_PUSH_SKIP");
    }
    return on;
}

// A graphics draw whose descriptor inputs equal the previous graphics draw's in this command buffer skips building its
// sets: every write would be the same, the push would be skipped and the allocated set reused without a bind (about a
// quarter of field and monsters2 draws, MKW_BIND_STATS). The inputs, compared and stored in place, are what
// bind_resources and its helpers read beyond the pipeline info's fixed bindings and Z's stand-ins:
// - the info (bindings, layouts), the program and its serial (the compiler-constant range of UBO slot 1);
// - vk_texture_slots_gen, vk_texture_epoch (a slot's image, view and once-per-submit storage check), vk_sampler_gen;
// - vk_mem_gen (buffer and offset of a span, mirror creation and per-submission sync);
// - the images in the feedback layout (the sampled layout);
// - per binding: the support inputs, c0's storage table and base instance, the bound uniform and storage ranges, the
//   texture and image handles (RsList; compared only when a bind op ran since the last draw, bind_gen);
// - storage buffer addresses read from user uniform blocks (RsList.p, ubo_ptr): compared on every draw, since the
//   block's contents change without a bind op.
// The side effects of a build repeat as no-ops in the same command buffer and submission: c0 and support buffers kept
// in B, note_written's ranges, vk_mem_gpu_wrote's write-backs, the per-submission mirror sync and dirty census note,
// the views cached per submission. The census counts are added again. A build that copied guest bytes into transient
// memory (an unaligned host range) is never skipped after: the copy may differ. B.res_valid says the sets bound hold
// the last build; vk_exec_draw_new_cb's memset clears it, and every build clears it until it ends.
// MKW_BIND_SKIP=0 builds every draw; =check builds every draw and stops when a draw with equal inputs gets other
// writes, census counts or descriptor commands.
static int bind_skip_on(void) {
    if (RS.on < 0) {
        const char *e = cfg_value("MKW_BIND_SKIP");
        RS.on = !strcmp(e, "check") ? 2 : strcmp(e, "0") != 0;
        if (RS.on != 1) rt_log("vk_exec: descriptor build skip %s", RS.on ? "check" : "off");
    }
    return RS.on;
}

static void rs_add_k(RsRange *r, uint32_t *n, const VkExec *ex, const void *at, size_t len, uint32_t uniform) {
    uint32_t off = (uint32_t)((const uint8_t *)at - (const uint8_t *)ex);
    if (*n && r[*n - 1].off + r[*n - 1].len == off && r[*n - 1].uniform == uniform) r[*n - 1].len += (uint32_t)len;
    else r[(*n)++] = (RsRange){off, (uint32_t)len, uniform};
}
static void rs_add(RsRange *r, uint32_t *n, const VkExec *ex, const void *at, size_t len) { rs_add_k(r, n, ex, at, len, 0); }

static const RsList *rs_list(const VkExec *ex, const VkPipeInfo *info) {
    if (info == rs_last) return rs_last_l;
    plat_mutex_lock(&rs_lock);
    if (!rs_lists.klen) nvn_hm_init(&rs_lists, sizeof info);
    RsList *l = nvn_hm_get(&rs_lists, &info);
    if (!l) {
        static RsRange tmp[MAX_DESC];
        RsPtr ptr[RS_PTRS];
        uint32_t n = 0, nd = 0, np = 0;
        int sup = 0, c0 = 0, ok = 1;
        for (uint32_t i = 0; i < info->n_bindings && ok; i++) {
            const VkPipeBinding *b = &info->bindings[i];
            const GfxShaderRes *r = b->res;
            uint32_t cnt = b->count ? b->count : 1;
            if ((nd += cnt) > MAX_DESC) { ok = 0; break; }   // the build stops on it
            int st = nvn_stage_of(b->stage);
            if (b->kind == VK_PIPE_SUPPORT) {
                sup = 1;
            } else if (b->kind == GFX_RES_UBO) {
                for (uint32_t k = 0; k < cnt; k++) {
                    int slot = r->slot + (int)k;
                    if (slot == 0 && c0_on()) c0 = 1, rs_add(tmp, &n, ex, ex->ssbo[st], sizeof ex->ssbo[st]);
                    else if (slot == 24 && c24_on()) rs_add(tmp, &n, ex, ex->c24[st], NVN_C24_BYTES);
                    else if (slot >= 3 && slot - 3 < 18) rs_add_k(tmp, &n, ex, &ex->ubo[st][slot - 3], sizeof(p_bind_buf), 1);
                }   // slot 1: the program (bind_sig); others: the zero buffer
            } else if (b->kind == GFX_RES_SSBO) {   // every descriptor of the binding is the same range
                int ubo, slot = ssbo_slot(r, st, &ubo);
                if (slot >= 0 && slot < 16) {
                    rs_add(tmp, &n, ex, ubo ? &ex->ubo[st][slot] : &ex->ssbo[st][slot], sizeof(p_bind_buf));
                } else if (ubo_ptr_res(r)) {
                    if (np == RS_PTRS) { ok = 0; break; }
                    rs_add(tmp, &n, ex, &ex->ubo[st][r->cbuf_slot - 3], sizeof(p_bind_buf));
                    ptr[np++] = (RsPtr){st, r};
                }
            } else if ((b->kind == GFX_RES_TEX || b->kind == GFX_RES_IMG) && b->type != VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER &&
                       b->type != VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
                int img = b->kind == GFX_RES_IMG, cap = img ? 32 : 128;
                for (uint32_t k = 0; k < cnt; k++) {
                    int slot = r->slot + (int)k;
                    if (slot >= 0 && slot < cap) rs_add(tmp, &n, ex, img ? &ex->img[st][slot] : &ex->tex[st][slot], 8);
                }
            }
        }
        uint64_t bytes = 0;
        for (uint32_t i = 0; i < n; i++) bytes += tmp[i].len;
        if (bytes > RS_BYTES) ok = 0;
        l = malloc(sizeof *l + sizeof *tmp * n);
        if (!l) rt_fatal("vk: out of memory");
        l->n = n, l->bytes = (uint32_t)bytes, l->sup = (uint8_t)sup, l->c0 = (uint8_t)c0, l->ok = (uint8_t)ok;
        l->np = (uint8_t)np;
        memcpy(l->p, ptr, sizeof *ptr * np);
        memcpy(l->r, tmp, sizeof *tmp * n);
        nvn_hm_put(&rs_lists, &info, l);
    }
    plat_mutex_unlock(&rs_lock);
    rs_last = info, rs_last_l = l;
    return l;
}

// The signature of a graphics draw's descriptor inputs into RS; 1 when it equals the stored one. The bound-slot bytes
// are compared only when a bind op ran since they were stored or the pipeline changed: otherwise they are the same.
// Check mode compares them anyway and stops when the bind op generation said they could not differ but they do.
static int bind_sig(const VkExec *ex, const VkPipeInfo *info) {
    const RsList *l = rs_list(ex, info);
    if (!l->ok) return RS.hn = 0, RS.info = NULL, RS.why = 5, 0;
    uint64_t *h = RS.h, diff = 0;
    uint32_t n = 0;
    int why = -1;   // the first word that differed
#define PUT(v) do { uint64_t v_ = (uint64_t)(v); if (why < 0 && (h[n] ^ v_)) why = (int)n; diff |= h[n] ^ v_; h[n++] = v_; } while (0)
    PUT((uintptr_t)info);
    PUT((uintptr_t)ex->prog);
    PUT(ex->prog->serial);
    PUT((uint64_t)vk_texture_slots_gen() << 32 | vk_texture_epoch());
    PUT((uint64_t)vk_sampler_gen() << 32 | (uint32_t)ex->nfbl);
    PUT(vk_mem_gen() + vk_usnap_gen());   // both only grow
    for (int i = 0; i < ex->nfbl && i < 9; i++) PUT((uintptr_t)ex->fbl_img[i]);
    if (l->sup) {
        float in[3];
        support_in(ex, in);
        uint64_t a;
        uint32_t ps;
        memcpy(&a, in, 8), memcpy(&ps, &in[2], 4);
        PUT(a);
        PUT(ps);
    }
    if (l->c0) PUT(c0_binst);
    for (int i = 0; i < l->np; i++) PUT(ubo_ptr(ex, l->p[i].st, l->p[i].res));
#undef PUT
    diff |= n != RS.hn;
    // words: 0 pipeline, 1-2 program, 3 slots, 4 samplers, 5 generations, then the rest (BS.miss's classes)
    static const int8_t cls[6] = {0, 1, 1, 2, 3, 4};
    RS.why = why >= 0 ? (why < 6 ? cls[why] : 5) : n != RS.hn ? 5 : -1;
    RS.hn = n;
    int fast = RS.info == info && RS.gen == bind_gen;
    if (fast && RS.on != 2) return !diff;
    const uint8_t *e = (const uint8_t *)ex;
    uint8_t *d = RS.body;
    int bd = 0, bdu = 1;   // bdu: only user uniform blocks' bindings differ (MKW_BIND_STATS)
    for (uint32_t i = 0; i < l->n; i++) {
        const RsRange *r = &l->r[i];
        if (!bd && memcmp(d, e + r->off, r->len)) bd = 1, bdu = r->uniform;
        else if (bd && bdu && !r->uniform && memcmp(d, e + r->off, r->len)) bdu = 0;   // ubo_rebind needs it exact
        if (bd) memcpy(d, e + r->off, r->len);
        d += r->len;
    }
    if (fast && bd) rt_fatal("vk_exec: descriptor build skip check: bound slots changed without a bind op");
    if (RS.why < 0 && bd) RS.why = bdu ? 7 : 6;
    RS.info = info, RS.gen = bind_gen;
    return !diff && !bd;
}

// MKW_BIND_XF: the draw's inputs as one key (the pipeline, the bound-slot bytes its descriptors read and the storage
// addresses taken from uniform blocks); 0 when they are not known or targets are in the feedback layout. The uniform
// blocks' own bindings are left out: their addresses move every frame (a linear allocator; 10 % equal frame to frame,
// recdiff.py) and their descriptors are resolved on every draw anyway.
static uint64_t xb_key(const VkExec *ex, const VkPipeInfo *info) {
    const RsList *l = rs_list(ex, info);
    if (!l->ok || ex->nfbl) return 0;
    uint64_t h = (uint64_t)(uintptr_t)info * 0x9E3779B97F4A7C15ull;
#define MIX(v) (h = (h ^ (uint64_t)(v)) * 0xBF58476D1CE4E5B9ull, h ^= h >> 31)
    const uint8_t *e = (const uint8_t *)ex;
    for (uint32_t i = 0; i < l->n; i++) {
        const RsRange *r = &l->r[i];
        if (r->uniform) continue;
        uint32_t o = 0;
        for (uint64_t w; o + 8 <= r->len; o += 8) memcpy(&w, e + r->off + o, 8), MIX(w);
        for (; o < r->len; o++) MIX(e[r->off + o]);
    }
    for (int i = 0; i < l->np; i++) MIX(ubo_ptr(ex, l->p[i].st, l->p[i].res));
#undef MIX
    return h | 1;
}

// Writes allocated set s from its packed descriptors d: through the layout's update template, or with the write
// structs w[i0..i1) when it has none.
static void write_set(const VkPipeInfo *info, uint32_t s, VkDescriptorSet set, const VkDescSlot *d, VkWriteDescriptorSet *w,
                      uint32_t i0, uint32_t i1) {
    if (i1 == i0) return;
    if (info->tpl[s]) {
        vk_pipeline_update_tpl_fn()(vk_ctx()->device, set, info->tpl[s], d);
        return;
    }
    for (uint32_t k = i0; k < i1; k++) w[k].dstSet = set;
    vkUpdateDescriptorSets(vk_ctx()->device, i1 - i0, w + i0, 0, NULL);
}

// Writes the pipeline's sets from the bound state: set 0, and set 1 with independent layouts. Each is pushed into the
// command buffer, or allocated, written and bound; a graphics set whose contents equal the last one allocated for that
// set number in this command buffer is reused, and bound again only when another layout or a push came between. A
// graphics push equal to the last one at its set number, with no other layout or bind between, is not recorded.
// The descriptors go into one packed array d, a VkDescSlot per descriptor in binding order, which a set's update
// template reads directly (VkPipeInfo.tpl); write structs are filled only when a set has no template.
// MKW_TRACE=draw: bind_resources by stage (TSC ticks), printed with prepare_draw's stages.
static _Thread_local uint64_t br_t;
_Thread_local uint64_t br_tsc[BR_N];
static _Thread_local int br_on;
#define BR_MARK(i) do { if (br_on) { uint64_t t_ = plat_cycles(); br_tsc[i] += t_ - br_t; br_t = t_; } } while (0)

typedef struct { VkWriteDescriptorSet w[MAX_BINDINGS]; VkDescSlot d[MAX_DESC]; VkBufferView bv[MAX_DESC]; } DrawBRA;
static _Thread_local DrawBRA *dtl_bra;
#define BRA (*dtl_bra)

typedef struct { const VkPipeInfo *info; uint32_t s; const VkDescSlot *d; VkWriteDescriptorSet *w; uint32_t i0, i1; } DcWrite;

static void dc_write(void *ctx, VkDescriptorSet set) {
    const DcWrite *c = ctx;
    write_set(c->info, c->s, set, c->d, c->w, c->i0, c->i1);
}

// The sets dc_bind bound last on this thread, with their dynamic offsets and their uniform buffers (ubo_rebind).
#define DC_DYN 64
typedef struct {
    const VkPipeInfo *info;              // graphics pipeline info of the sets below; NULL: none to rebind
    VkDescriptorSet set[2];              // the graphics sets bound with them (B.ds_bound may hold a compute one since)
    uint32_t n[2];
    uint32_t dyn[2][DC_DYN];
    VkBuffer buf[2][DC_DYN];
    VkDeviceSize range[2][DC_DYN];
} DcBound;
static _Thread_local DcBound DCB;

// Binds set s through the cache: the uniform buffers' offsets leave the descriptors (made 0) for the bind's dynamic
// offsets, in binding order.
static VkDescriptorSet dc_bind(VkExec *ex, const VkPipeInfo *info, uint32_t s, VkDescSlot *ds, uint32_t n,
                               VkWriteDescriptorSet *w, uint32_t i0, uint32_t i1) {
    if (ru_cap) ru_note_set(info, s, ds, n, (uint32_t)(ds - BRA.d), i0, i1);
    VkBuffer ubuf[MAX_DESC];
    VkDeviceSize urange[MAX_DESC];
    uint32_t dyn[MAX_DESC], ndyn = 0;
    for (uint32_t i = i0, at = 0; i < i1; i++) {
        uint32_t cnt = info->bindings[i].count ? info->bindings[i].count : 1;
        if (info->bindings[i].type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)
            for (uint32_t k = 0; k < cnt; k++) {
                VkDeviceSize off = ds[at + k].buffer.offset;
                if (off >> 32) rt_fatal("vk: draw cache: a uniform buffer offset of %#llx does not fit a dynamic offset",
                                        (unsigned long long)off);
                ubuf[ndyn] = ds[at + k].buffer.buffer, urange[ndyn] = ds[at + k].buffer.range;
                dyn[ndyn++] = (uint32_t)off;
                ds[at + k].buffer.offset = 0;
            }
        at += cnt;
    }
    DcWrite cw = {info, s, ds, w, i0, i1};
    VkDescriptorSet set = vk_dcache_get(s ? info->set_layout1 : info->set_layout, ds, n, ex->fence, dc_write, &cw);
    // The same set with the same offsets already bound at s (B.ds_bound is cleared by a new layout or command buffer):
    // nothing to record, as the push path skipped an equal push.
    int gfx = info->bind_point == VK_PIPELINE_BIND_POINT_GRAPHICS;
    if (gfx && s < 2 && ndyn <= DC_DYN) {
        memcpy(DCB.buf[s], ubuf, ndyn * sizeof *ubuf), memcpy(DCB.range[s], urange, ndyn * sizeof *urange);
        DCB.set[s] = set;
        if (s == 0) DCB.info = info;
    } else if (gfx) {
        DCB.info = NULL;
    }
    if (B.ds_bound[s] == set && DCB.n[s] == ndyn && ndyn <= DC_DYN && !memcmp(DCB.dyn[s], dyn, ndyn * sizeof *dyn)) {
        if (vk_guard_on()) vk_guard_set_hit(ex->cb, info->bind_point, s, set);
        BS.push_skip++;
        return set;
    }
    vkCmdBindDescriptorSets(ex->cb, info->bind_point, info->layout, s, 1, &set, ndyn, dyn);
    B.ds_bound[s] = ndyn <= DC_DYN ? set : VK_NULL_HANDLE, B.ds_pushed[s] = 0;
    if (ndyn <= DC_DYN) DCB.n[s] = ndyn, memcpy(DCB.dyn[s], dyn, ndyn * sizeof *dyn);
    RS.cmds++;
    return set;
}

// MKW_TRACE_BINDS=1 (diagnosis): per draw, its replay position, program and what each binding names in terms stable
// from one run to the next (a buffer's guest address and range, a texture's game handle and the layout given), so two
// recording orders of a capture show the first draw that binds something else (grabacion-paralela, the title's fault).
static void trace_binds(const VkExec *ex, const VkPipeInfo *info, const VkDescSlot *d) {
    static int on = -1;
    if (on < 0) on = cfg_get("MKW_TRACE_BINDS") && atoi(cfg_get("MKW_TRACE_BINDS"));
    if (!on) return;
    char b[2048];
    uint64_t ph = 0xcbf29ce484222325ull;   // the NVN-side pipeline state (dynamic state included), stable across runs
    for (size_t i = 0; i < sizeof ex->pipe; i++) ph = (ph ^ ((const uint8_t *)&ex->pipe)[i]) * 0x100000001b3ull;
    int k = snprintf(b, sizeof b, "binds: op %llu prog %llu pipe %016llx rt %d:", (unsigned long long)vk_exec_op_pos(),
                     (unsigned long long)(ex->prog ? ex->prog->serial : 0), (unsigned long long)ph, ex->in_pass);
    for (uint32_t i = 0, at = 0; i < info->n_bindings && k < (int)sizeof b - 64; i++) {
        const VkPipeBinding *pb = &info->bindings[i];
        uint32_t cnt = pb->count ? pb->count : 1;
        int st = nvn_stage_of(pb->stage), slot = pb->res ? pb->res->slot : -1;
        if (pb->kind == GFX_RES_UBO && slot >= 3 && slot - 3 < 18)
            k += snprintf(b + k, sizeof b - (size_t)k, " u%d.%d=%#llx+%llu", st, slot, (unsigned long long)ex->ubo[st][slot - 3].addr,
                          (unsigned long long)d[at].buffer.range);
        else if (pb->kind == GFX_RES_SSBO && slot >= 0 && slot < 16)
            k += snprintf(b + k, sizeof b - (size_t)k, " s%d.%d=%#llx+%llu", st, slot, (unsigned long long)ex->ssbo[st][slot].addr,
                          (unsigned long long)d[at].buffer.range);
        else if (pb->kind == GFX_RES_SSBO && pb->res && ubo_ptr_res(pb->res))   // an address the shader reads from a UBO
            k += snprintf(b + k, sizeof b - (size_t)k, " p%d.c%d=%#llx+%llu", st, pb->res->cbuf_slot,
                          (unsigned long long)ubo_ptr((VkExec *)ex, st, pb->res), (unsigned long long)d[at].buffer.range);
        else if (pb->kind == GFX_RES_SSBO)
            k += snprintf(b + k, sizeof b - (size_t)k, " s%d.?+%llu", st, (unsigned long long)d[at].buffer.range);
        else if (pb->kind == GFX_RES_TEX && slot >= 0 && slot < 128)
            k += snprintf(b + k, sizeof b - (size_t)k, " t%d.%d=%#llx/%d", st, slot, (unsigned long long)ex->tex[st][slot],
                          (int)d[at].image.imageLayout);
        at += cnt;
    }
    rt_log("%s", b);
}

// MKW_UBO_SKIP (default 1; "ab"), with MKW_DRAW_CACHE: ubo_rebind below.
static int ubo_skip_on(void) {
    static int mode = -1;
    if (mode < 0) {
        mode = vk_pipeline_draw_cache() ? rt_knob("UBO_SKIP", "1") : 0;
        if (vk_pipeline_draw_cache())
            rt_log("vk_exec: draws that change only their uniform blocks rebind the last sets %s (MKW_UBO_SKIP)",
                   mode == 2 ? "in the B blocks" : mode ? "on" : "off");
    }
    return RT_KNOB_ON(mode);
}

// MKW_DRAW_CACHE: a graphics draw whose descriptor inputs equal the previous draw's but for its user uniform blocks'
// bindings (bind_sig, RS.why 7: 46-49 % of the race's draws, bs3) rebinds the sets dc_bind bound last with the blocks'
// new dynamic offsets, when each block lies in the same buffer with the same range; 0: nothing recorded, build.
static int ubo_rebind(VkExec *ex, const VkPipeInfo *info) {
    if (DCB.info != info || B.ds_layout != info->layout) return 0;
    uint32_t n_sets = info->n_sets ? info->n_sets : 1, nd[2] = {0, 0}, dyn[2][DC_DYN];
    int has[2] = {0, 0};
    for (uint32_t i = 0; i < info->n_bindings; i++) {
        const VkPipeBinding *b = &info->bindings[i];
        uint32_t s = n_sets == 1 ? 0 : b->set, cnt = b->count ? b->count : 1;
        has[s] = 1;
        if (b->type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) continue;
        for (uint32_t k = 0; k < cnt; k++) {
            VkDescriptorBufferInfo bi;
            if (b->kind == VK_PIPE_SUPPORT) bi = (VkDescriptorBufferInfo){B.sup_buf, B.sup_off, SUPPORT_BYTES};
            else if (b->kind == GFX_RES_UBO) bi = ubo_info(ex, nvn_stage_of(b->stage), b->res->slot + (int)k);
            else return 0;
            uint32_t j = nd[s]++;
            if (j >= DCB.n[s] || bi.buffer != DCB.buf[s][j] || bi.range != DCB.range[s][j] || bi.offset >> 32) return 0;
            dyn[s][j] = (uint32_t)bi.offset;
        }
    }
    for (uint32_t s = 0; s < n_sets; s++)
        if (has[s] && (nd[s] != DCB.n[s] || !DCB.set[s] || B.ds_bound[s] != DCB.set[s])) return 0;
    for (uint32_t s = 0; s < n_sets; s++) {
        if (!has[s] || !memcmp(dyn[s], DCB.dyn[s], nd[s] * sizeof **dyn)) continue;
        vkCmdBindDescriptorSets(ex->cb, info->bind_point, info->layout, s, 1, &DCB.set[s], nd[s], dyn[s]);
        memcpy(DCB.dyn[s], dyn[s], nd[s] * sizeof **dyn);
    }
    return 1;
}

void bind_resources(VkExec *ex, const VkPipeInfo *info) {
    br_on = (rt_trace & RT_TRACE_DRAW) != 0;
    if (br_on) br_t = plat_cycles();
    VkWriteDescriptorSet *w = BRA.w;
    VkDescSlot *d = BRA.d;
    VkBufferView *bv = BRA.bv;
    if (info->n_bindings > MAX_BINDINGS) rt_fatal("vk: %u descriptor bindings exceed the replay's %d", info->n_bindings, MAX_BINDINGS);
    bind_stats(info);
    int gfx = info->bind_point == VK_PIPELINE_BIND_POINT_GRAPHICS;
    int skip = gfx ? bind_skip_on() : 0, same = 0, was_valid = B.res_valid;
    if (skip) {
        same = bind_sig(ex, info);
        if (!same && bind_stats_on() && RS.why >= 0) BS.miss[RS.why]++;
        if (same && was_valid && skip == 1) {
            for (int i = 0; i < RS.ncnt; i++) vk_mem_count(RS.cnt[i].cls, RS.cnt[i].use, RS.cnt[i].bytes);
            BS.bind_skip++;
            ex->recorded++;
            return;
        }
        if (!same && RS.why == 7 && was_valid && skip == 1 && ubo_skip_on() && ubo_rebind(ex, info)) {
            for (int i = 0; i < RS.ncnt; i++) vk_mem_count(RS.cnt[i].cls, RS.cnt[i].use, RS.cnt[i].bytes);
            BS.ubo_skip++;
            ex->recorded++;
            return;
        }
        B.res_valid = 0;   // until the build below ends
        if (skip == 2) memcpy(RS.pcnt, RS.cnt, sizeof RS.cnt), RS.pncnt = RS.ncnt;
        RS.rec = 1, RS.ncnt = 0, RS.cnt_over = 0, RS.volatile_ = 0, RS.cmds = 0;
    }
    int xb = gfx ? xb_mode() : 0;
    uint64_t xkey = 0;
    const VkDescSlot *xhad = NULL;
    static _Thread_local uint8_t xkeep[MAX_DESC];
    if (xb && (xkey = xb_key(ex, info))) {
        uint32_t ndesc = 0;
        for (uint32_t i = 0; i < info->n_bindings; i++) ndesc += info->bindings[i].count ? info->bindings[i].count : 1;
        xhad = xb_get(xkey, info, ndesc);
    }
    const VkDescSlot *xuse = xb == 2 ? xhad : NULL;
    BR_MARK(BR_SIG);
    uint32_t n_sets = info->n_sets ? info->n_sets : 1;
    uint32_t lo[2] = {0, 0}, hi[2] = {0, 0}, d0[2] = {0, 0};   // bindings [lo, hi) and first descriptor per set
    int need_w = 0;                                             // a set without a template: fill the write structs
    for (uint32_t s = 0, i0 = 0; s < n_sets; s++) {
        uint32_t i1 = i0;
        while (i1 < info->n_bindings && (n_sets == 1 || info->bindings[i1].set == s)) i1++;
        lo[s] = i0, hi[s] = i1, i0 = i1;
        if (i1 > lo[s] && !info->tpl[s]) need_w = 1;
    }
    uint32_t nd = 0;
    for (uint32_t i = 0; i < info->n_bindings; i++) {
        const VkPipeBinding *b = &info->bindings[i];
        const GfxShaderRes *r = b->res;
        uint32_t cnt = b->count ? b->count : 1;
        if (nd + cnt > MAX_DESC) rt_fatal("vk: %u descriptors exceed the replay's %d", nd + cnt, MAX_DESC);
        if (n_sets == 2 && i == lo[1]) d0[1] = nd;
        VkDescSlot *o = &d[nd];
        int st = nvn_stage_of(b->stage), kind = 0;   // kind: 0 buffer, 1 image, 2 texel buffer view
        if (xkey) memset(xkeep + nd, 0, cnt);
        if (b->kind == VK_PIPE_SUPPORT) {
            o->buffer = support_info(ex);
            if (ru_cap) ru_note_desc(nd, RUK_SUP, 0, 0);
            BR_MARK(BR_C0);
        } else if (b->kind == GFX_RES_UBO) {
            for (uint32_t k = 0; k < cnt; k++) {
                o[k].buffer = ubo_info(ex, st, r->slot + (int)k);
                if (ru_cap) ru_note_desc(nd + k, ubo_kind, st, r->slot + (int)k - 3);
                BR_MARK(r->slot + (int)k < 3 ? BR_C0 : BR_UBO);
            }
        } else if (b->kind == GFX_RES_SSBO) {
            if (xuse) memcpy(o, xuse + nd, sizeof *o * cnt);
            else
                for (uint32_t k = 0; k < cnt; k++) o[k].buffer = ssbo_info(ex, st, r, r->flags & 1);
            if (xkey) memset(xkeep + nd, 1, cnt);
            BR_MARK(BR_SSBO);
        } else if (b->kind == GFX_RES_TEX || b->kind == GFX_RES_IMG) {
            int img = b->kind == GFX_RES_IMG;
            if (b->type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || b->type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
                kind = 2;
                for (uint32_t k = 0; k < cnt; k++) {   // buffer textures are not replayed yet
                    o[k].w[0] = 0, o[k].w[1] = 0, o[k].w[2] = 0;
                    o[k].view = Z.zview;
                    if (need_w) bv[nd + k] = Z.zview;
                }
            } else if (xuse) {
                kind = 1;
                memcpy(o, xuse + nd, sizeof *o * cnt);
                memset(xkeep + nd, 1, cnt);
            } else {
                kind = 1;
                if (xkey) memset(xkeep + nd, 1, cnt);
                int locked = 0;   // tex_info takes the texture lock for the lookups that need it
                for (uint32_t k = 0; k < cnt; k++) {
                    int slot = r->slot + (int)k;
                    uint64_t h = 0;
                    if (img) h = slot >= 0 && slot < 32 ? ex->img[st][slot] : 0;
                    else h = slot >= 0 && slot < 128 ? ex->tex[st][slot] : 0;
                    tex_info(ex, r, h, img, r->separate, &o[k].image, &locked);
                    if (ru_cap && !img && slot >= 0 && slot < 128) ru_note_slot(0, st, slot);   // others read 0
                    o[k].w[2] = (uint32_t)o[k].image.imageLayout;   // zero the tail padding
                }
                if (locked) vk_texture_unlock();
            }
            BR_MARK(BR_TEX);
        } else {
            rt_fatal("vk: binding %u has an unknown kind %d", (unsigned)b->binding, (int)b->kind);
        }
        if (need_w) {
            w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                          .dstBinding = b->binding, .descriptorCount = cnt, .descriptorType = b->type};
            if (kind == 0) w[i].pBufferInfo = &o->buffer;   // a VkDescSlot is as large as either info struct
            else if (kind == 1) w[i].pImageInfo = &o->image;
            else w[i].pTexelBufferView = &bv[nd];
        }
        nd += cnt;
    }
    if (n_sets == 2 && lo[1] == info->n_bindings) d0[1] = nd;   // set 1 empty: set 0 runs to the end (run42: its size was 0)
    // A null handle where the descriptor type needs one makes the driver read through NULL (RADV 26.2 inside
    // vkCmdPushDescriptorSetWithTemplateKHR, run39/run40): stop with the binding instead.
    for (uint32_t i = 0, at = 0; i < info->n_bindings; i++) {
        const VkPipeBinding *b = &info->bindings[i];
        uint32_t cnt = b->count ? b->count : 1;
        for (uint32_t k = 0; k < cnt; k++) {
            const VkDescSlot *o = &d[at + k];
            int bad;
            switch (b->type) {
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: bad = !o->buffer.buffer; break;
            case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: bad = !o->image.imageView || !o->image.sampler; break;
            case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE: case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: bad = !o->image.imageView; break;
            case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER: case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: bad = !o->view; break;
            default: bad = 0;
            }
            if (bad)
                rt_fatal("vk: null descriptor: binding %u[%u] set %u kind %d type %d stage %d slot %d (%#llx %#llx %#llx)",
                         (unsigned)b->binding, (unsigned)k, (unsigned)b->set, (int)b->kind, (int)b->type, (int)b->stage,
                         b->res ? b->res->slot : -1, (unsigned long long)o->w[0], (unsigned long long)o->w[1],
                         (unsigned long long)o->w[2]);
        }
        at += cnt;
    }
    // Under the validation layer, a new graphics layout binds set 1 before set 0 is pushed or bound. The layer (SDK
    // 1.4.363, PipelineLayoutCompatDef::operator==) takes any two INDEPENDENT_SETS layouts as compatible, so
    // vkCmdPushDescriptorSetKHR keeps the push set it made for an earlier layout and drops writes to bindings that
    // layout lacks (false VUID-vkCmdDrawIndexed-None-08114, then an access violation inside the layer). A bind at set 1
    // disturbs set 0 by its canonical id, which drops the layer's push set: the push after it gets a new one.
    if (xkey) xb_put(xkey, info, d, nd, xkeep, xb == 1 ? xhad : NULL);
    BR_MARK(BR_CHECK);
    vk_dcache_probe(info, d, nd);
    trace_binds(ex, info, d);
    int set1_first = 0;
    if (gfx && info->layout != B.ds_layout) {   // another layout: sets bound before may be disturbed
        B.ds_layout = info->layout;
        B.ds_bound[0] = B.ds_bound[1] = VK_NULL_HANDLE;
        B.ds_pushed[0] = B.ds_pushed[1] = 0;
        set1_first = n_sets == 2 && vk_ctx()->validation;
    }
    for (uint32_t k = 0; k < n_sets; k++) {
        uint32_t s = set1_first ? 1 - k : k, i0 = lo[s], i1 = hi[s];
        const VkDescSlot *ds = &d[d0[s]];
        uint32_t dbytes = (uint32_t)(((s + 1 < n_sets ? d0[s + 1] : nd) - d0[s]) * sizeof *d);
        if (s && i1 == i0) {               // no fragment stage: set 1 is empty and unused
            if (set1_first) {              // bound only to drop the layer's push set (comment above)
                VkDescriptorSet set = alloc_set(ex, info->set_layout1);
                vkCmdBindDescriptorSets(ex->cb, info->bind_point, info->layout, 1, 1, &set, 0, NULL);
                B.ds_bound[1] = set, B.ds_pushed[1] = 0;
                RS.cmds++;
            }
            continue;
        }
        if (vk_pipeline_draw_cache()) {
            dc_bind(ex, info, s, (VkDescSlot *)ds, dbytes / (uint32_t)sizeof *d, w, i0, i1);
            continue;
        }
        if (s ? info->push1 : info->push) {
            uint32_t n = gfx && push_skip_on() ? dbytes : 0;
            if (n && B.ds_pushed[s] && DS.pn[s] == n && !memcmp(DS.psig[s], ds, n)) {
                if (vk_guard_on()) vk_guard_set_hit(ex->cb, info->bind_point, s, VK_NULL_HANDLE);
                BS.push_skip++;
                continue;
            }
            if (info->tpl[s] && vk_guard_on()) vk_guard_pushed(ex->cb, info->bind_point, s);   // the template has no bp
            if (info->tpl[s]) rec_push_tpl(vk_pipeline_push_tpl_fn(), ex->cb, info->tpl[s], info->layout, s, ds, dbytes);
            else rec_push(vk_pipeline_push_fn(), ex->cb, info->bind_point, info->layout, s, i1 - i0, w + i0);
            BS.push++;
            RS.cmds++;
            if (gfx) {
                B.ds_bound[s] = VK_NULL_HANDLE, B.ds_pushed[s] = n != 0;
                if (n) DS.pn[s] = n, memcpy(DS.psig[s], ds, n);
            }
        } else if (!gfx || s > 1) {
            VkDescriptorSet set = alloc_set(ex, s ? info->set_layout1 : info->set_layout);
            write_set(info, s, set, ds, w, i0, i1);
            vkCmdBindDescriptorSets(ex->cb, info->bind_point, info->layout, s, 1, &set, 0, NULL);
        } else {
            VkDescriptorSetLayout sl = s ? info->set_layout1 : info->set_layout;
            VkDescriptorSet set = B.ds_last[s];
            if (!set || B.ds_last_layout[s] != sl || DS.n[s] != dbytes || memcmp(DS.sig[s], ds, dbytes)) {
                set = alloc_set(ex, sl);
                write_set(info, s, set, ds, w, i0, i1);
                B.ds_last[s] = set, B.ds_last_layout[s] = sl, DS.n[s] = dbytes;
                memcpy(DS.sig[s], ds, dbytes);
                BS.set_new++;
                RS.cmds++;
            } else {
                BS.set_reuse++;
            }
            if (B.ds_bound[s] != set) {
                vkCmdBindDescriptorSets(ex->cb, info->bind_point, info->layout, s, 1, &set, 0, NULL);
                B.ds_bound[s] = set, B.ds_pushed[s] = 0;
                RS.cmds++;
            } else if (vk_guard_on()) {
                vk_guard_set_hit(ex->cb, info->bind_point, s, set);
            }
        }
    }
    if (skip) {
        RS.rec = 0;
        // An equal next draw records nothing when every push set holds its push (MKW_PUSH_SKIP on) and nothing made
        // the build unrepeatable; allocated sets are always bound to the one written last.
        int valid = !RS.volatile_ && !RS.cnt_over && RS.hn;
        for (uint32_t s = 0; s < n_sets; s++)
            if (hi[s] > lo[s] && (s ? info->push1 : info->push) && !B.ds_pushed[s]) valid = 0;
        if (skip == 2) {   // with the pipeline info equal (bind_sig), the packed descriptors determine every write
            uint32_t n = (uint32_t)(nd * sizeof *d);
            int wr = n != RS.ckn || memcmp(RS.ck, d, n);
            int cn = RS.ncnt != RS.pncnt || memcmp(RS.cnt, RS.pcnt, sizeof RS.cnt[0] * (size_t)RS.ncnt);
            if (same && was_valid && (wr || cn || RS.cmds))
                rt_fatal("vk_exec: descriptor build skip check: a draw of program %llu with the inputs of the previous one "
                         "gets %s", (unsigned long long)ex->prog->serial,
                         RS.cmds ? "descriptor commands" : wr ? "other writes" : "other census counts");
            if (same && was_valid) BS.bind_skip++;   // the draws a skip would have spared, compared instead
            RS.ckn = n;
            memcpy(RS.ck, d, n);
        }
        B.res_valid = valid;
    }
    BR_MARK(BR_SETS);
    ex->recorded++;
}

struct BindTls {
    DrawDS ds;
    DrawRS rs;
    DrawUPD upd;
    DrawBRA bra;
    TexMemo td[TD_N];
};

// This thread's descriptor state (vk_exec_draw_thread).
void vk_exec_bind_thread(void) {
    struct BindTls *t = calloc(1, sizeof *t);
    if (!t) rt_fatal("vk: out of memory (replay thread state)");
    t->rs.on = -1;
    dtl_ds = &t->ds, dtl_rs = &t->rs, dtl_upd = &t->upd, dtl_bra = &t->bra, TD = t->td;
}

void vk_exec_bind_shutdown(void) {
    nvn_hm_clear(&rs_lists, free);
    rs_last = NULL, rs_last_l = NULL;
}

// ---- MKW_REUSE (vk_exec_reuse.c): a reused draw's sets bound again

int ru_ubo_ok(VkExec *ex, int st, int slot) {
    if (!__atomic_load_n(&LIM.ubo_max, __ATOMIC_ACQUIRE)) limits_init();
    const p_bind_buf *b = &ex->ubo[st][slot];
    VkDescriptorBufferInfo bi;
    return b->size && vk_usnap_find(b->addr, b->size < LIM.ubo_max ? b->size : LIM.ubo_max, LIM.ubo_align, &bi);
}

// The descriptors of the cached draw's set s, with its c0, support and snapshot uniform blocks made again for this
// command buffer and submission, bound through the draw cache as dc_bind binds a replayed draw's. The set the last hit
// bound (rs) is bound again with the new dynamic offsets when every descriptor made again names the buffer and range it
// named then (those not dynamic, also the offset) and the draw cache has not emptied since: no hash of the
// descriptors, no lookup under the cache's lock (ru21: the sets were 2 700 of the hit's 9 000 ticks).
static uint64_t rs_mix(uint64_t h, uint64_t w) { return (h ^ w) * 0xBF58476D1CE4E5B9ull ^ ((h ^ w) >> 31); }

int ru_bind_set(VkExec *ex, const VkPipeInfo *info, uint32_t s, VkDescSlot *ds, uint32_t n, uint32_t i0, uint32_t i1,
                const uint8_t *kind, const uint8_t *kst, const uint8_t *kslot, uint32_t c0_binst_, RuSetMemo *rs) {
    if (!__atomic_load_n(&LIM.ubo_max, __ATOMIC_ACQUIRE)) limits_init();
    cb_res(ex);
    for (uint32_t j = 0; j < n; j++) {
        switch (kind[j]) {
        case RUK_C0: c0_binst = c0_binst_, ds[j].buffer = c0_info(ex, kst[j]); break;
        case RUK_C24: ds[j].buffer = c24_info(ex, kst[j]); break;
        case RUK_SUP: ds[j].buffer = support_info(ex); break;
        case RUK_UBO: {
            const p_bind_buf *b = &ex->ubo[kst[j]][kslot[j]];
            VkDescriptorBufferInfo bi;
            if (!b->size || !vk_usnap_find(b->addr, b->size < LIM.ubo_max ? b->size : LIM.ubo_max, LIM.ubo_align, &bi))
                return 0;
            ds[j].buffer = bi;
            break;
        }
        default: break;
        }
    }
    uint32_t dyn[MAX_DESC], ndyn = 0;
    uint64_t sig = (uint64_t)(uintptr_t)info->layout;
    for (uint32_t i = i0, at = 0; i < i1; i++) {
        uint32_t cnt = info->bindings[i].count ? info->bindings[i].count : 1;
        int dynamic = info->bindings[i].type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        for (uint32_t k = 0; k < cnt; k++) {
            const VkDescriptorBufferInfo *b = &ds[at + k].buffer;
            if (dynamic) {
                if (b->offset >> 32) return dc_bind(ex, info, s, ds, n, NULL, i0, i1), rs->set = VK_NULL_HANDLE, 1;
                dyn[ndyn++] = (uint32_t)b->offset;
                sig = rs_mix(rs_mix(sig, (uint64_t)(uintptr_t)b->buffer), b->range);
            } else if (kind[at + k] != RUK_STABLE) {
                sig = rs_mix(rs_mix(rs_mix(sig, (uint64_t)(uintptr_t)b->buffer), b->offset), b->range);
            }
        }
        at += cnt;
    }
    uint64_t epoch = vk_dcache_epoch();
    if (rs->set && rs->sig == sig && rs->epoch == epoch) {
        vkCmdBindDescriptorSets(ex->cb, info->bind_point, info->layout, s, 1, &rs->set, ndyn, dyn);
        B.ds_bound[s] = VK_NULL_HANDLE, B.ds_pushed[s] = 0;   // the caller forgets the replay's bindings after the hit
        RS.cmds++;
        return 2;
    }
    rs->set = dc_bind(ex, info, s, ds, n, NULL, i0, i1), rs->sig = sig, rs->epoch = epoch;
    return 1;
}
