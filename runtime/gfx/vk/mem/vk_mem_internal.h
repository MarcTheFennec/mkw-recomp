// Internal interface of the Vulkan memory layer (vk_memory.c and vk_mem_*.c): the pool record and the state the parts
// share. Everything else goes through vk.h.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "gfx.h"
#include "host.h"
#include "nvn_pool_index.h"
#include "vk.h"
#include "platform/platform.h"

// Timestamps for the tracking cost, in ns; qpc_ms converts.
static inline uint64_t qpc(void) { return plat_now_ns(); }

static inline double qpc_ms(uint64_t ticks) { return (double)ticks / 1e6; }

enum { POOL_IMPORT = 1, POOL_PHYS, POOL_VIRT };
enum { MS_NONE, MS_ON, MS_REFUSED };

typedef struct { struct PoolRec *phys; uint64_t off; } VPage;   // virtual page -> physical pool and offset
typedef struct { VkBuffer buf; VmaAllocation alloc; VkDeviceMemory mem; uint64_t use; } PhysChunk;   // POOL_PHYS: device memory (or UMA imported host memory) of one chunk; use: vk_guard_note

typedef struct PoolRec {
    const pool_t *key;
    int kind;
    uint32_t flags;
    uint64_t lo, size;                 // GPU address range of the pool
    uint64_t import_lo, import_hi;     // POOL_IMPORT: the imported host range (aligned outwards)
    VkBuffer buf;
    VkDeviceMemory mem;                // POOL_IMPORT
    PhysChunk *chunks;                 // POOL_PHYS: size / phys_chunk() entries, made at first use (phys_back)
    uint32_t nchunks;
    uint8_t *pseen;                    // POOL_PHYS: 64 KiB pages some MapVirtual request has named (phys_seen)
    VPage *pt;                         // POOL_VIRT: size / 64 KiB entries
    uint64_t npages;
    volatile int32_t used;                // resolved at least once: the GPU may still read it at finalize
    uint64_t use;                      // MKW_GPU_GUARD: newest submission part that resolved it (vk_guard_note)
    // MKW_TRACE=perf dirty census (vk_mem_dirty_submit), replay thread under the submit lock only:
    uint32_t bound_sub;                // last submit that bound it as a buffer (dirty_sub), 0 never
    uint32_t synced_gen;               // page generation its lazy-mirror count covers; DIRTY_UNSYNCED until scanned
    uint8_t bufbound;                  // ever bound as a vertex, index, uniform, storage or indirect buffer
    uint8_t untracked;                 // the last dirty scan found no write watch over it
    // VRAM mirror (POOL_IMPORT, "VRAM mirror" below). mstate and mbuf change on the replay thread only.
    uint8_t mstate;                    // MS_NONE until the first buffer use, then MS_ON or MS_REFUSED
    VkBuffer mbuf;                     // MS_ON: device-local copy of [import_lo, import_hi), same offsets as buf
    VmaAllocation malloc_;
    uint32_t acc_sub;                  // replay: the last submission that synced the mirror (mirror_sync)
    uint32_t wb_sub;                   // replay: the last submission that wrote mirror bytes back to guest memory
    uint32_t mgen;                     // gen_lock: every page write stamped up to this generation is in the mirror
                                       // (or in a copy queued before its next use)
    uint64_t fl_lo, fl_hi;             // flush_lock: nvnMemoryPoolFlushMappedRange bytes not copied yet (fl_hi 0: none)
    // Shadow storage (POOL_IMPORT in shadow mode, "shadow storage" below): buf is a host-visible copy of the guest pages.
    VmaAllocation salloc;
    uint8_t *smap;                     // its mapping; offsets as in buf (from import_lo)
    uint32_t sgen;                     // gen_lock: every page write stamped up to this generation is in the shadow
    volatile uint8_t stouched;         // on the touched list: the GPU may use it in the submission being recorded
    uint64_t ssub;                     // shadow_lock: the submission whose shadow sync last covered it
    uint64_t sfence;                   // touch_lock: the submission recording when it went on the early list
    uint8_t shist;                     // gen_lock: one bit per shadow sync of the last 8, set when the CPU had written it
    uint8_t shot;                      // gen_lock: syncs left that query it without protecting again (MKW_SHADOW_HOT)
    uint64_t touch_sub;                // shadow_lock: the submit number of the last sync that had it touched
    uint64_t cap_from;                 // gen_lock: the first submit captured for it (UINT64_MAX: none yet)
    uint32_t cgen;                     // gen_lock: every page stamped up to this generation is captured
    uint8_t cap_on;                    // shadow_lock: on the capture list (vk_mem_capture.c)
    uint8_t placed;                    // POOL_IMPORT as driver memory mapped at the guest range (placed_pool): mem
    uint8_t lazy;                      // POOL_IMPORT whose GPU storage is made at its first resolve (lazy_back)
} PoolRec;

// vk_memory.c: the pool index
extern PlatMutex pool_lock;
extern uint64_t pool_gen, mirror_gen;
extern pidx pool_index;
extern int32_t n_imports, n_import_failures;
int rec_class(const PoolRec *r);

// vk_mem_back.c: GPU storage of each kind of pool
extern PlatMutex phys_lock;
extern int64_t phys_backed_n, phys_backed_bytes, phys_failed_n, placed_n, placed_bytes;
VkBufferUsageFlags pool_usage(const VkCtx *c);
uint64_t phys_chunk(void);
int phys_lazy(void);
uint64_t chunk_len(const PoolRec *r, uint32_t i);
void phys_back(PoolRec *r, uint32_t first, uint32_t last);
int import_pool(VkCtx *c, PoolRec *r, uint64_t addr, uint64_t size);
int lazy_on(void);
void lazy_back(PoolRec *r);
void destroy_rec(VkCtx *c, PoolRec *r);

// vk_mem_gen.c: page write generations and write-watch snapshots
#define GEN_CHUNK_SHIFT 28
#define GEN_PAGE_SHIFT 12
#define GEN_CHUNKS (RT_ADDRESS_LIMIT >> GEN_CHUNK_SHIFT)
#define GEN_PER_CHUNK (1u << (GEN_CHUNK_SHIFT - GEN_PAGE_SHIFT))
#define REG_SHIFT 21
#define SNAP_SHIFT 21
#define SNAP_REGIONS (RT_ADDRESS_LIMIT >> SNAP_SHIFT)
extern PlatMutex gen_lock;
extern uint32_t *gen_chunk[GEN_CHUNKS];
extern uint32_t gen_now;
extern uint32_t reg_max[RT_ADDRESS_LIMIT >> REG_SHIFT];
extern uint32_t snap_at[SNAP_REGIONS];
extern uint8_t snap_untracked[SNAP_REGIONS];
extern uint64_t ww_bytes, ww_pages;
extern int ww_peek, freeze_idle;
extern uint32_t ww_ep;
void gen_lock_x(void);
void gen_lock_s(void);
uint32_t *gen_slot(uint64_t page_addr, int create);
int query_stamp(uint64_t lo, uint64_t hi, uint32_t g);
uint64_t snap_regions(uint64_t r0, uint64_t r1, uint32_t epoch);
void freeze_pass(void);
void ww_log(uint64_t frame);
void gen_reset(void);

// vk_mem_dirty.c: dirty census (MKW_TRACE=perf)
void dirty_frame(uint64_t frame);
int vk_mem_is_discrete(void);

// vk_mem_shadow.c: shadow storage
extern int shadow_on, read_traps, wb_defer;
extern int64_t s_live_n, s_live_bytes;
int shadow_pool(VkCtx *c, PoolRec *r, uint64_t addr, uint64_t size);
void shadow_touch(PoolRec *r);
void shadow_forget(PoolRec *r);
void shadow_log(uint64_t frame);
void shadow_reset(void);

// vk_mem_merge.c: the shadow merged by bytes (MKW_SHADOW_MERGE), with a base copy of each written range up to MERGE_MAX
#define MERGE_MAX (4ull << 20)
uint8_t *merge_base(const PoolRec *r, uint64_t lo, uint64_t hi);
extern uint64_t merge_over;   // shadow_lock: bytes merge_in gave to the CPU over the GPU's (MKW_SHADOW_CPU_WINS)
uint64_t merge_in(PoolRec *r, uint64_t a, uint64_t b, uint8_t *const *bases, const uint64_t *blo, int n,
                  const uint8_t *src);
typedef struct LazyBase LazyBase;   // vk_mem_lazy.c: a base standing on read-only guest pages
uint64_t merge_back(uint64_t addr, uint64_t size, const uint8_t *s, const uint8_t *base, LazyBase *lazy);
int lazy_base_on(void);                                                 // MKW_LAZY_BASE for a base taken now
LazyBase *lazy_take(PoolRec *r, uint64_t lo, uint64_t hi);         // shadow_lock; NULL: copy it as before
void lazy_materialize(LazyBase *b, uint64_t a, uint64_t e);        // [a, e) into its buffer before writing the base
uint8_t *lazy_check(LazyBase *b);                                  // MKW_LAZY_BASE_CHECK's copy, or NULL
uint8_t *lazy_buf(LazyBase *b);                                    // the base, [lo, hi) of the range
int lazy_page_base(LazyBase *b, uint64_t x, const uint8_t *s, uint64_t n, uint8_t *tmp);
void lazy_free(LazyBase *b);
void lazy_log(void);
void merge_census_log(void);   // MKW_WB_CENSUS

// vk_mem_wback.c: the GPU's writes into shadow pools, pending from record time until written back at their fence.
// part: the sync that submits it (s_part when recorded); pool and mbase: the merge's; base: vk_mem_lost.c's.
// tnote: when noted; eother: the early thread left its base for another part's pending write over it, the same
// range's (2) or another (1) (the line «bases by none», vk_mem_shadow.c); btried: its base was taken or could not be.
typedef struct {
    uint64_t lo, hi, fence, part;
    PoolRec *pool;
    uint8_t *mbase, *base;
    LazyBase *lazy;                      // mbase is its buffer (vk_mem_lazy.c)
    uint8_t merge, eother, btried;
    uint64_t tnote;
} SRange;
extern SRange *spend;                    // shadow_lock: GPU writes recorded and not copied back yet
extern int nspend;
extern PlatMutex touch_lock;             // innermost: after shadow_lock (vk_mem_shadow.c)
extern PlatCond touch_cv;
extern uint64_t s_part, s_wb_bytes;      // shadow_lock (s_part also under touch_lock when it changes)
extern int32_t n_mbase;                  // shadow_lock (read without it): pending writes with a merge base
extern int n_want;                       // shadow_lock: this part's merge ranges without a base
extern volatile int32_t want_new;        // a merge range was noted since the early thread looked
extern int early_mode, merge_mode;
void spend_take(void);                   // shadow_lock held: the writes noted since into spend
void spend_take_locked(void);            // shadow_lock and touch_lock held
void sdone_drain(void);                  // shadow_lock held: noted writes in, written-back ones out
void wback_reset(void);                  // shadow_lock held (vk_mem_shutdown)

// vk_mem_shadow.c parts that vk_mem_capture.c uses
extern PlatMutex shadow_lock;
extern int hot_mode;
void shadow_query_pools(PoolRec **rs, uint8_t *tracked, int n, int hot_on);   // gen_lock held
void shadow_apply_run(PoolRec *r, uint64_t a, uint64_t b, const uint8_t *src);   // shadow_lock held

// vk_mem_capture.c: the CPU's writes captured at each submit (MKW_SHADOW_SUBMIT)
void cap_apply(uint64_t sub);                    // shadow_lock held: captures of submits up to sub into the shadows
int cap_split(PoolRec **rs, int n);              // shadow_lock and gen_lock held: captured pools last; how many not
void cap_activate(PoolRec **rs, int n);          // shadow_lock and gen_lock held: those pools onto the capture list
void cap_forget(PoolRec *r);                     // shadow_lock held: r leaves the list and the pending captures
void cap_log(void);                              // shadow_lock held

// vk_mem_lost.c: MKW_SHADOW_LOST, what the shadow loses of the CPU's writes (diagnosis)
extern int lost_on;   // -1 until lost_level reads the switch
int lost_level(void);
void lost_skip(const PoolRec *r, uint64_t lo, uint64_t hi, int old);
uint8_t *lost_base(uint64_t addr, uint64_t size);
void lost_writeback(uint64_t addr, uint64_t size, const uint8_t *s, uint8_t *base);
void lost_check(void);

// vk_mem_race.c: MKW_SHADOW_RACE, shadow copies over bytes a running submission reads (diagnosis)
extern int race_on;   // -1 until race_level reads the switch
int race_level(void);
void race_note(const PoolRec *r, uint64_t lo, uint64_t hi);              // recording thread, at a resolve
// shadow_lock held: the newest unfinished fence reading bytes of [a, b) that the copy changes, 0 none
uint64_t race_check(const PoolRec *r, uint64_t a, uint64_t b, uint64_t sync, const uint8_t *old);
void race_wait(uint64_t fence);   // shadow_lock held: MKW_FENCE_EARLY, before a copy race_check flagged
void race_log(uint64_t frame);
extern uint64_t watch_hi;   // MKW_SHADOW_WATCH: nonzero when some range's shadow changes are logged
void watch_init(void);
void watch_copy(const PoolRec *r, uint64_t a, uint64_t b, uint64_t sync);   // shadow_lock held, before a copy-in
void watch_event(const char *what, uint64_t lo, uint64_t hi, uint64_t fence, uint64_t bytes);

// vk_mem_mirror.c: VRAM mirror of buffer-bound pools
extern int mirror_verify, mirror_gap;
extern volatile int64_t m_live_n, m_live_bytes;
void mirror_forget(PoolRec *r);     // pool_lock exclusive: off the mirrored list
void mirror_retiring(PoolRec *r);   // vk_pool_fini: kept until mirror_retire (or vk_mem_shutdown)
void mirror_retire(void *arg);      // completion thread: destroys a finalized mirrored pool's record
int retiring_count(void);
void mirror_log(uint64_t frame);
void mirror_reset(VkCtx *c);

// vk_mem_log.c: census, VRAM summary and the memory mode
void flag_names(uint32_t f, char *buf, size_t n);
void vram_log(uint64_t frame);

// vk_mem_upload.c: upload batches and deferred destruction
void vk_mem_grow(void **arr, int *cap, size_t elem);
int dead_pending(void);
uint64_t upload_batches(void);
// vk_mem_uptime.c: MKW_UPLOAD_TIME, the GPU time of each upload batch
int vk_uptime_begin(VkCommandBuffer cb);
void vk_uptime_call(int slot, void *at);
void vk_uptime_end(VkCommandBuffer cb, int slot);
void vk_uptime_done(int slot, uint64_t seq, uint64_t stg, uint64_t texels, uint32_t calls);
void upload_shutdown(VkCtx *c);
