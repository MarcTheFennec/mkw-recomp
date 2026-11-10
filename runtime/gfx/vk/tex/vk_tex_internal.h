// Internal interface of the texture cache (vk_texture.c and vk_tex_*.c): the entry and slot records and the state the
// parts share. Everything else goes through vk.h.
#pragma once

#include <stdint.h>

#include "gfx.h"
#include "nvn_hmap.h"
#include "nvn_tex.h"
#include "tex.h"
#include "vk.h"
#include "platform/platform.h"

typedef struct {
    uint64_t addr;
    nvn_texdesc d;
    uint8_t has_layout, layout[8], pad[7];
} TexKey;

typedef struct Tex {
    VkTexImage pub;               // first: vk_texture_get hands out &pub
    TexKey key;
    tex_t t;                      // the newest texture object seen with this key
    TexVkFormat f;
    VmaAllocation alloc;          // own memory; NULL when bound into pool memory
    VmaAllocation bound_to;       // the pool memory it is bound into
    uint32_t wgen;                // page generation of the last upload check
    uint32_t checked;             // submit_epoch of the last check from a draw binding
    uint32_t seen;                // submit_epoch of the last lookup of any kind (evict_idle: idle since then)
    int uploaded, released;
    uint32_t n_up;                // uploads of this entry (vk_texture_debug)
    int last_ch;                  // last vk_mem_changed result of make_fresh: 1 written, 0 clean, -1 not tracked
    struct Tex *next_at;          // other entries at the same storage address
    uint8_t gpu_dirty;            // mirrored, and the GPU wrote the image after the last write-back to the storage
    uint32_t gpu_gen;             // page generation when the GPU write that set gpu_dirty was recorded
    uint64_t gpu_seq;             // order of that GPU write among all of them (gpu_writes)
    uint64_t gpu_fence;           // fence of the submission that made that write (0: not recorded in one)
    uint64_t wr_fence;            // any entry: newest submission recorded to write its image (render, copy, clear,
                                  // storage binding); ReadTexels waits for it instead of draining the replay queue
    uint8_t listed;               // in owned[]
    uint32_t wt_n;                // WriteTexels into the storage since the last upload
    struct Tex *rel_prev, *rel_next;   // while released: the list of released entries, oldest first
    uint8_t sparse;               // a Vulkan sparse image whose tiles follow the game's MapVirtual (vk_sparse.c)
    uint8_t sparse_fallback;      // a non-sparse VMA-backed image for a sparse texture whose format has no sparse blocks
    uint64_t use;                 // MKW_GPU_GUARD: newest submission part that looked it up (vk_guard_note)
} Tex;

// A slot keeps what vk_texture_by_id resolved for it, so a draw binding skips the key lookup and the address chain: the
// entry (ce), valid while the generation of its address (agen) is still cgen, the view and has_view handed out with it,
// and the submit_epoch of the last freshness check through the slot (cepoch). Hot fields first: a lookup that hits
// touches one or two cache lines.
typedef struct {
    struct Tex *ce;               // NULL: nothing cached (registered since, or never resolved)
    uint32_t cgen;                // agen of the slot's address when ce was resolved
    uint32_t cepoch;
    view_t cview;
    uint8_t chas_view, used, has_view;
    view_t v;
    tex_t t;
} TexSlot;
typedef struct { samp_par p; VkSampler s, sp; uint8_t used; } SampSlot;   // s: vk_sampler_for(&p) once looked up; sp:
                                                                           // its point-sampled form (vk_sampler_point_by_id_locked)

// vk_texture.c: the cache state, under tex_lock unless noted
#define AGEN_N 4096
extern PlatRMutex tex_lock;
extern PlatOnce tex_once;
extern volatile int32_t tex_ready;
extern nvn_hmap by_key, by_addr, samplers;
extern TexSlot *tslots;
extern int ntslots;
extern SampSlot *sslots;
extern int nsslots, nreleased;
extern Tex *rel_head, *rel_tail;
extern uint64_t n_uploads, n_reuploads, ru_n, ru_bytes, ru_ns, ru_wt_n, ru_wt_bytes, ru_wt_ns;
extern int tex_alias;
extern uint64_t n_alias_copied, n_alias_upload, n_alias_uninit;
extern uint32_t submit_epoch, tex_gen, slots_gen, samp_gen, reg_serial;
extern PlatMutex owned_lock;
extern Tex **owned;
extern int nowned, cowned;
extern uint64_t max_owned, gpu_writes, n_checks;
extern uint32_t agen[AGEN_N];
extern uint64_t *sinfo, *schk;
void tex_init(void *p);
uint32_t agen_idx(uint64_t addr);
void tex_gen_bump(uint64_t addr);
void make_key(const tex_t *t, TexKey *k);
int same_but_swizzle(const TexKey *a, const TexKey *b);
int tex_mirrored(const tex_t *t);
void log_limited(volatile int32_t *n, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void barrier(VkCommandBuffer cb, VkPipelineStageFlags2 src, VkAccessFlags2 sa, VkPipelineStageFlags2 dst,
             VkAccessFlags2 da);
#define ALL_RW VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT

static inline void lock(void) {
    if (!__atomic_load_n(&tex_ready, __ATOMIC_ACQUIRE)) plat_once(&tex_once, tex_init, NULL);
    plat_rmutex_lock(&tex_lock);
}
static inline void unlock(void) { plat_rmutex_unlock(&tex_lock); }

static inline uint32_t umax(uint32_t a, uint32_t b) { return a > b ? a : b; }
static inline uint32_t mip(uint32_t v, unsigned lv) { return umax(v >> lv, 1); }

// vk_tex_image.c: images
Tex *create_tex(const tex_t *t, const TexKey *k, int defer_init);
void init_image(Tex *e, VkCommandBuffer cb);
VkResult vma_image(VkCtx *c, const VkImageCreateInfo *ci, const VmaAllocationCreateInfo *ai, VkImage *img,
                   VmaAllocation *alloc);   // in the texture pool when it fits

// vk_tex_evict.c: released entries, eviction and the owned list
void destroy_tex(Tex *e);
void destroy_idle(Tex *e);   // one the submission being recorded does not use (released or idle): freed sooner
VkResult evict_idle(VkCtx *c, const VkImageCreateInfo *ci, const VmaAllocationCreateInfo *ai, VkImage *img,
                    VmaAllocation *alloc);
void own_add(Tex *e);
int owned_lower(uint64_t a);
void rel_append(Tex *e);
void rel_unlink(Tex *e);
void sweep_released(void);

// vk_tex_upload.c: uploads and the freshness checks
void make_fresh(Tex *e);
void make_fresh_snap(Tex *e);
void expand_rgb(int format, const uint8_t *src, uint8_t *dst, size_t n);
void shrink_rgb(int format, const uint8_t *src, uint8_t *dst, size_t n);
int storage_rewritten(const Tex *e, uint32_t since, uint32_t *now);
extern uint64_t trace_addr;
int tracing(uint64_t addr);

// vk_tex_alias.c: aliases (MKW_TEX_ALIAS) and GPU writes into mirrored storage
enum { WB_SMALL = 64 << 10 };
extern int small_pending;
Tex *entry_of(const tex_t *t);
Tex *entries_at(uint64_t addr);   // the first entry at that storage address; the rest by next_at

// vk_tex_texel.c: texel reads and writes
typedef struct { unsigned level, layer0; int nz, is3d, bw, bh, bpb; size_t wb, hb, row, img, x0, y0; } Region;
int region_of(const tex_t *t, const view_t *v, const NVNcopyRegion *r, uint64_t row, uint64_t img, Region *g);
int image_texels(Tex *e, const Region *g, const NVNcopyRegion *r, uint8_t *mem, int write);
int storage_texels(const tex_t *t, const Region *g, const NVNcopyRegion *r, uint8_t *mem, int write);
