// Vulkan memory of the graphics layer: NVN memory pools as Vulkan buffers (the pool index, virtual-pool page table and
// GPU address resolution here). The parts, sharing vk_mem_internal.h: vk_mem_back.c (each pool's GPU storage),
// vk_mem_gen.c (page write generations for re-uploads), vk_mem_shadow.c, vk_mem_mirror.c, vk_mem_dirty.c,
// vk_mem_log.c (census, memory mode) and vk_mem_upload.c (the upload batch that vk_texture.c records into).
//
// Behaviour ported from MacNativeGames games/totk/third_party/d2r_gfx/gfx/nvn/totk_vpool.c (request checks, unmap
// semantics, 64 KiB pages) and nvn_dirty.c (generation stamps: a texture is stale when a covered page carries a
// generation newer than its upload), @ b75be8cf (MIT, Copyright (c) 2026 jhaizhou; origin d2r-mobile); and from LAS
// Recomp runtime/nvn/vulkan/exec.c:2094-2131 (vk_pages_changed: every write-watch query resets the OS bits, so each
// query stamps the pages it saw with a new generation shared by all users) and textures.c:240-270 (deferred
// destruction) @ da35b83a (MIT, Copyright (c) 2026 Peiikai). MNG aliases host VM (mach_vm_remap) and protects pages
// (fault handler); here virtual pools are a GPU-side page table over device-local memory and dirty pages come from
// GetWriteWatch (mem_written_pages), so neither is needed.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "nvn_api.h"
#include "nvn_pool_index.h"
#include "vk.h"
#include "vk_exec.h"
#include "vk_mem_internal.h"
#include "vk_reuse.h"
#include "platform/platform.h"

PlatMutex pool_lock = PLAT_MUTEX_INIT;
// Bumped under pool_lock (exclusive) by every change to the index or a page table: vk_mem_resolve's per-thread hits
// stay valid while it is unchanged.
uint64_t pool_gen = 1;
// Bumped when a buffer use may get another buffer for the same span (a mirror created, refused or forgotten) and at
// every submission's start (mirror syncs are per submission). vk_mem_gen: both together.
uint64_t mirror_gen;
pidx pool_index;                // POOL_IMPORT, POOL_PHYS and POOL_VIRT records by GPU address
static PoolRec **phys_list;            // physical pools by key, for MapVirtual requests
static int nphys, cap_phys;
int32_t n_imports, n_import_failures;

static void index_put(PoolRec *r) {
    __atomic_add_fetch(&pool_gen, 1, __ATOMIC_RELEASE);
    pidx_ent e = {.lo = r->lo, .hi = r->lo + r->size, .base = r->lo, .buf = r, .gpu = r->lo, .key = r->key};
    pidx_remove(&pool_index, r->key, NULL);   // an object initialized again without Finalize
    if (!pidx_insert(&pool_index, &e)) rt_fatal("vk: out of memory for the pool index");
}

static PoolRec *phys_find(const pool_t *key) {
    for (int i = 0; i < nphys; i++) if (phys_list[i]->key == key) return phys_list[i];
    return NULL;
}
int vk_pool_init(pool_t *p) {
    VkCtx *c = vk_ctx();
    if (!c) { rt_log("vk: MemoryPoolInitialize before vk_init"); return 0; }
    // The GPU never sees a GPU_NO_ACCESS pool, except a physical one (0x189; 1.4.3 also 0x18c, CPU-visible) through the
    // virtual pools that map it. Either gets device-local memory: 1.4.3's CPU never writes its 0x18c pool's storage
    // (unchanged over 3600 frames of the intro), so the guest storage and the GPU copy need no sync.
    if ((p->flags & NVN_MEMORY_POOL_FLAGS_GPU_NO_ACCESS) && !(p->flags & NVN_MEMORY_POOL_FLAGS_PHYSICAL)) return 1;
    PoolRec *r = calloc(1, sizeof *r);
    if (!r) rt_fatal("vk: out of memory for a pool record");
    r->key = p;
    r->flags = p->flags;
    r->lo = (uintptr_t)p->mem;
    r->size = p->size;
    int ok;
    if (p->flags & NVN_MEMORY_POOL_FLAGS_VIRTUAL) {
        r->kind = POOL_VIRT;
        r->npages = p->size / NVN_VPOOL_PAGE;
        r->pt = calloc(r->npages ? r->npages : 1, sizeof *r->pt);
        ok = r->pt != NULL;
    } else if (p->flags & NVN_MEMORY_POOL_FLAGS_PHYSICAL) {
        r->kind = POOL_PHYS;   // device memory chunk by chunk at first use (phys_back)
        uint64_t ch = phys_chunk();
        r->nchunks = (uint32_t)(ch >= r->size ? 1 : (r->size + ch - 1) / ch);
        r->chunks = calloc(r->nchunks, sizeof *r->chunks);
        r->pseen = calloc(r->size / NVN_VPOOL_PAGE, 1);
        ok = r->chunks && r->pseen;
    } else if (lazy_on()) {
        r->kind = POOL_IMPORT;   // GPU storage at the first resolve (lazy_back); never, if the GPU never reads it
        r->lazy = 1;
        ok = 1;
    } else {
        r->kind = POOL_IMPORT;
        mem_thaw((uintptr_t)p->mem, p->size);   // the import may need write access to the pages
        ok = import_pool(c, r, (uintptr_t)p->mem, p->size);
        if (!ok) {
            // Not fatal for the game: the pool works for the CPU, and vk_mem_resolve reports its addresses as unbacked.
            long n = plat_inc32(&n_import_failures);
            if (n <= 8) rt_log("vk: pool %p (flags %#x, %#llx bytes) has no GPU buffer (%ld so far)", (void *)p->mem,
                               p->flags, (unsigned long long)p->size, n);
            r->buf = VK_NULL_HANDLE;
            ok = 1;
        }
    }
    if (!ok) { free(r->pt); free(r->chunks); free(r->pseen); free(r); return 0; }
    plat_mutex_lock(&pool_lock);
    index_put(r);
    if (r->kind == POOL_PHYS) {
        if (nphys == cap_phys) {
            cap_phys = cap_phys ? cap_phys * 2 : 16;
            phys_list = realloc(phys_list, (size_t)cap_phys * sizeof *phys_list);
            if (!phys_list) rt_fatal("vk: out of memory for the physical pool list");
        }
        phys_list[nphys++] = r;
    }
    plat_mutex_unlock(&pool_lock);
    return 1;
}

// The last pools finalized (pool_lock): a replay that resolves an address in one finds no pool. With the asynchronous
// replay a finalize no longer drains the queue (vk_backend.c), on the NVN rule that the game frees a pool only after
// waiting for the GPU's last use of it; a queued submission that still names one is counted and logged here.
#define GONE_N 64
typedef struct { uint64_t lo, hi; } GoneRange;
static GoneRange gone[GONE_N];
static uint32_t gone_at;
static volatile int32_t n_gone_hits;

int vk_pool_needs_drain(const pool_t *p) {
    plat_mutex_lock_shared(&pool_lock);
    const pidx_ent *e = pidx_find(&pool_index, (uintptr_t)p->mem, 0);
    const PoolRec *r = e && e->key == p ? e->buf : NULL;
    int need = r && ((r->kind == POOL_IMPORT && r->mem) || (r->kind == POOL_PHYS && !vk_mem_is_discrete()));   // imported or placed: its GPU storage is guest memory
    plat_mutex_unlock_shared(&pool_lock);
    return need;
}

void vk_pool_fini(pool_t *p) {
    VkCtx *c = vk_ctx();
    if (!c) return;
    pidx_ent e;
    plat_mutex_lock(&pool_lock);
    int had = pidx_remove(&pool_index, p, &e);
    __atomic_add_fetch(&pool_gen, 1, __ATOMIC_RELEASE);
    PoolRec *r = had ? e.buf : NULL;
    if (r) gone[gone_at++ % GONE_N] = (GoneRange){r->lo, r->lo + r->size};
    if (r && r->mstate == MS_ON) mirror_forget(r);
    if (r && r->kind == POOL_PHYS) {
        for (int i = 0; i < nphys; i++)
            if (phys_list[i] == r) { phys_list[i] = phys_list[--nphys]; break; }
        for (int i = 0; i < pool_index.n; i++) {   // virtual pages that still map it become unmapped
            PoolRec *v = pool_index.e[i].buf;
            if (v->kind != POOL_VIRT) continue;
            for (uint64_t k = 0; k < v->npages; k++)
                if (v->pt[k].phys == r) v->pt[k].phys = NULL;
        }
    }
    plat_mutex_unlock(&pool_lock);
    if (!r) return;
    if (r->smap) shadow_forget(r), mem_untrap(r->import_lo, r->import_hi - r->import_lo);   // the GPU's bytes, now
    if (r->kind == POOL_IMPORT) mem_thaw(r->lo, r->size);   // the storage goes back to the guest writable
    // Images cut from this range go first; a used buffer or memory waits until the GPU is done with it, since the
    // guest gets the storage back when this returns.
    vk_texture_range_gone(r->lo, r->size, NULL);
    for (uint32_t i = 0; i < r->nchunks; i++)
        if (r->chunks[i].alloc) vk_texture_range_gone(0, 0, r->chunks[i].alloc);
    // Only an import's GPU storage is guest memory. A shadow or a physical pool's storage is ours: it goes once the
    // submissions issued so far are done, without holding the game (the idle wait cost ~24 ms per finalize while
    // loading a course, live35).
    int guest_storage = r->kind == POOL_IMPORT && r->mem;   // not a shadow, nor a lazy pool never backed
    for (uint32_t i = 0; i < r->nchunks && !guest_storage; i++)
        if (r->chunks[i].mem) guest_storage = 1;
    if (r->used && guest_storage) {
        vk_upload_flush(1);
        vk_queue_lock();
        vkQueueWaitIdle(c->gfx_queue);
        vk_queue_unlock();
    } else if (r->used && r->kind != POOL_VIRT) {
        vk_upload_flush(1);
    }
    vk_mem_collect();
    // A mirrored pool's record and buffers outlive the submission being recorded, whose sync copies and write-backs may
    // name them (the game frees a pool only once the GPU is done with it, but its CPU writes may still be queued).
    // A shadow or physical pool's storage goes once the GPU is done with what was submitted: the game used the pool last
    // before its wait, so neither the submission being recorded nor a queued one names it. Waiting for the next
    // submission instead held freed storage while loading, and a staging buffer found the host heap full (rt-b0).
    if (r->mbuf) mirror_retiring(r), vk_completion_add(vk_sync_issued() + 1, mirror_retire, r);
    else if (r->used && !guest_storage && r->kind != POOL_VIRT) {
        if (vk_guard_on()) {
            uint64_t part, f = vk_sync_issued_pair(&part);
            vk_guard_tie("pool", (uint64_t)(uintptr_t)r->buf, r->lo, r->use, part);
            for (uint32_t i = 0; i < r->nchunks; i++)
                if (r->chunks[i].buf)
                    vk_guard_tie("physical pool chunk", (uint64_t)(uintptr_t)r->chunks[i].buf, r->lo + i * phys_chunk(),
                                 r->chunks[i].use, part);
            mirror_retiring(r), vk_completion_add(f, mirror_retire, r);
        } else {
            mirror_retiring(r), vk_completion_add(vk_sync_issued(), mirror_retire, r);
        }
    }
    else destroy_rec(c, r);
}

// Distinct physical pages the game has mapped, against what the pools reserve (pool_lock exclusive).
static int64_t phys_seen_pages, map_bytes, unmap_bytes, map_calls;

static void phys_seen(PoolRec *ph, uint64_t off, uint64_t size) {
    for (uint64_t k = off / NVN_VPOOL_PAGE; k < (off + size) / NVN_VPOOL_PAGE; k++)
        if (!ph->pseen[k]) ph->pseen[k] = 1, phys_seen_pages++;
}

// Requests were checked by the front end (nvn_api_core.c, MNG mkw_vpool.c); apply them in order. The physical chunks
// they name get their memory first, outside pool_lock.
int vk_pool_map_virtual(pool_t *v, int n, const void *requests) {
    const nvn_mapping_t *rq = requests;
    static int sparse_trace = -1;
    if (sparse_trace < 0) sparse_trace = cfg_get("MKW_SPARSE_TRACE") != NULL;
    for (int i = 0; i < n; i++) {
        if (!rq[i].physical_pool) continue;
        plat_mutex_lock_shared(&pool_lock);
        PoolRec *ph = phys_find((const pool_t *)(const void *)rq[i].physical_pool);
        plat_mutex_unlock_shared(&pool_lock);
        if (!ph) continue;   // reported below
        uint64_t ch = phys_chunk(), off = (uint64_t)rq[i].physical_offset;
        if (!phys_lazy()) phys_back(ph, (uint32_t)(off / ch), (uint32_t)((off + rq[i].size - 1) / ch));
    }
    plat_mutex_lock(&pool_lock);
    const pidx_ent *e = pidx_find(&pool_index, (uintptr_t)v->mem, 0);
    PoolRec *r = e && e->key == v ? e->buf : NULL;
    int ok = r && r->kind == POOL_VIRT;
    __atomic_add_fetch(&pool_gen, 1, __ATOMIC_RELEASE);
    for (int i = 0; ok && i < n; i++) {
        PoolRec *ph = rq[i].physical_pool ? phys_find((const pool_t *)(const void *)rq[i].physical_pool) : NULL;
        if (rq[i].physical_pool && !ph) {
            rt_log("vk: MapVirtual request %d names physical pool %p that has no Vulkan memory", i,
                   (const void *)rq[i].physical_pool);
            ok = 0;
            break;
        }
        uint64_t first = (uint64_t)rq[i].virtual_offset / NVN_VPOOL_PAGE, count = rq[i].size / NVN_VPOOL_PAGE;
        if (first + count > r->npages) { ok = 0; break; }
        for (uint64_t k = 0; k < count; k++)
            r->pt[first + k] = (VPage){ph, ph ? (uint64_t)rq[i].physical_offset + k * NVN_VPOOL_PAGE : 0};
        if (ph) phys_seen(ph, (uint64_t)rq[i].physical_offset, rq[i].size), map_bytes += (int64_t)rq[i].size;
        else unmap_bytes += (int64_t)rq[i].size;
        if (sparse_trace)   // tools/gpu/sparse_fit.py: which pages the game maps inside its sparse textures
            rt_log("sparse: map %#llx +%#llx -> %#llx", (unsigned long long)(r->lo + (uint64_t)rq[i].virtual_offset),
                   (unsigned long long)rq[i].size,
                   ph ? (unsigned long long)(ph->lo + (uint64_t)rq[i].physical_offset) : 0ull);
    }
    int64_t calls = ++map_calls, seen = phys_seen_pages, mb = map_bytes, ub = unmap_bytes;
    uint64_t vlo = ok ? r->lo : 0;
    plat_mutex_unlock(&pool_lock);
    if (!ok) rt_log("vk: MapVirtual on pool %p failed", (void *)v->mem);
    for (int i = 0; ok && i < n; i++) vk_sparse_remap(vlo + (uint64_t)rq[i].virtual_offset, rq[i].size);
    if (!(calls & (calls - 1)) || !(calls % 4096)) {
        plat_mutex_lock(&phys_lock);
        int64_t backed = phys_backed_bytes;
        plat_mutex_unlock(&phys_lock);
        rt_log("vk: MapVirtual %lld calls: %.1f MiB mapped, %.1f MiB unmapped; %.1f MiB of physical pages named, "
               "%.1f MiB of chunks backed", (long long)calls, mb / 1048576.0, ub / 1048576.0,
               seen * (double)NVN_VPOOL_PAGE / 1048576.0, backed / 1048576.0);
    }
    return ok;
}

// Per-thread copies of resolved import and physical pools that overlap no other index entry (so pidx_find picks them for
// any range inside), valid while pool_gen is unchanged: the replay resolves a few dozen pools thousands of times per
// frame (16 vertex streams and every buffer binding of each draw). Direct-mapped by the 4 KiB page of the address, so
// a hit is one compare however many pools a frame touches. MKW_RESOLVE_CHECK=1 compares every hit with the locked
// lookup and stops on a difference.
int rec_class(const PoolRec *r) {
    if (r->kind != POOL_IMPORT) return VK_MCLS_DEVICE;
    return r->flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS ? VK_MCLS_HOST_NOACC
         : r->flags & NVN_MEMORY_POOL_FLAGS_CPU_CACHED ? VK_MCLS_HOST_CACHED : VK_MCLS_HOST_UNC;
}

typedef struct { uint64_t gen, lo, hi, base, end; VkBuffer buf; VmaAllocation alloc; int cls; void *pool; } ResolveHit;
#define RHIT_SLOTS 512
static _Thread_local ResolveHit rhit[RHIT_SLOTS];

static ResolveHit *rhit_slot(uint64_t addr) { return &rhit[(addr >> 12 ^ addr >> 21) & (RHIT_SLOTS - 1)]; }

static int resolve_check_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = cfg_get("MKW_RESOLVE_CHECK");
        on = e && *e && strcmp(e, "0");
    }
    return on;
}

static int resolve_locked(uint64_t addr, uint64_t size, VkMemSpan *out);

int vk_mem_resolve(uint64_t addr, uint64_t size, VkMemSpan *out) {
    memset(out, 0, sizeof *out);
    uint64_t g = __atomic_load_n(&pool_gen, __ATOMIC_ACQUIRE), end = addr + size;
    const ResolveHit *h = rhit_slot(addr);
    if (end >= addr && h->gen == g && addr >= h->lo && end <= h->hi) {
        out->buffer = h->buf;
        out->offset = addr - h->base;
        out->avail = h->end - addr;
        out->alloc = h->alloc;
        out->cls = h->cls;
        out->pool = h->pool;
        if (h->alloc) out->alloc_offset = out->offset;
        if (vk_guard_on() && h->pool) vk_guard_note(&((PoolRec *)h->pool)->use);
        if (shadow_on && h->pool && ((PoolRec *)h->pool)->smap && !((PoolRec *)h->pool)->stouched) shadow_touch(h->pool);
        if (race_on > 0 && h->pool && ((PoolRec *)h->pool)->smap) race_note(h->pool, addr, end);
        if (resolve_check_on()) {
            VkMemSpan s;
            if (!resolve_locked(addr, size, &s) || memcmp(&s, out, sizeof s))
                rt_fatal("vk: resolve cache of 0x%llx differs from the pool index", (unsigned long long)addr);
        }
        if (ru_cap) ru_note_mem(addr, size, out, 1);
        return 1;
    }
    int ok = resolve_locked(addr, size, out);
    if (ru_cap) ru_note_mem(addr, size, out, ok);   // MKW_REUSE: what a captured draw resolved
    return ok;
}

// The pool index lookup under the lock; a non-overlapped import or physical pool goes into the address's cache slot.
static int resolve_locked(uint64_t addr, uint64_t size, VkMemSpan *out) {
    memset(out, 0, sizeof *out);
    plat_mutex_lock_shared(&pool_lock);
    const pidx_ent *e = pidx_find(&pool_index, (uintptr_t)addr, size);
    PoolRec *r = e ? e->buf : NULL;
    if (r && r->lazy) {   // first GPU use: make its storage under the exclusive lock, then look it up again
        plat_mutex_unlock_shared(&pool_lock);
        plat_mutex_lock(&pool_lock);
        e = pidx_find(&pool_index, (uintptr_t)addr, size);
        r = e ? e->buf : NULL;
        if (r && r->lazy) lazy_back(r);
        plat_mutex_unlock(&pool_lock);
        plat_mutex_lock_shared(&pool_lock);
        e = pidx_find(&pool_index, (uintptr_t)addr, size);
        r = e ? e->buf : NULL;
    }
    if (r && r->kind == POOL_PHYS && !r->chunks[(addr - r->lo) / phys_chunk()].buf) {   // a direct use: back its chunk
        uint32_t i = (uint32_t)((addr - r->lo) / phys_chunk());
        plat_mutex_unlock_shared(&pool_lock);
        phys_back(r, i, i);
        plat_mutex_lock_shared(&pool_lock);
        e = pidx_find(&pool_index, (uintptr_t)addr, size);
        r = e ? e->buf : NULL;
    }
    int ok = 0;
    if (r && r->kind == POOL_IMPORT && r->buf != VK_NULL_HANDLE) {
        int i = (int)(e - pool_index.e);
        if ((i == 0 || pool_index.maxhi[i - 1] <= e->lo) && (i + 1 == pool_index.n || pool_index.e[i + 1].lo >= e->hi)) {
            ResolveHit *h = rhit_slot(addr);
            h->gen = __atomic_load_n(&pool_gen, __ATOMIC_RELAXED);   // stable: changed only under the exclusive lock
            h->lo = e->lo, h->hi = e->hi, h->buf = r->buf, h->cls = rec_class(r);
            h->base = r->import_lo, h->end = r->import_hi, h->alloc = NULL, h->pool = r;
        }
    }
    if (r && r->kind == POOL_IMPORT && r->buf) {
        out->cls = rec_class(r);
        out->pool = r;
        out->buffer = r->buf;
        out->offset = addr - r->import_lo;
        out->avail = r->import_hi - addr;
        if (r->smap && !r->stouched) shadow_touch(r);
        if (race_on > 0 && r->smap) race_note(r, addr, addr + size);
        if (vk_guard_on()) vk_guard_note(&r->use);
        ok = 1;
    } else if (r && r->kind == POOL_PHYS) {
        uint64_t rel = addr - r->lo, ch = phys_chunk(), i = rel / ch;
        PhysChunk *k = &r->chunks[i];
        if (k->buf) {
            if (vk_guard_on()) vk_guard_note(&r->use), vk_guard_note(&k->use);
            out->buffer = k->buf;
            out->offset = out->alloc_offset = rel - i * ch;
            out->avail = chunk_len(r, (uint32_t)i) - out->offset;
            out->alloc = k->alloc;
            out->cls = VK_MCLS_DEVICE;
            ok = out->avail >= size;
        }
    } else if (r && r->kind == POOL_VIRT) {
        uint64_t rel = addr - r->lo, k = rel / NVN_VPOOL_PAGE;
        PoolRec *ph = k < r->npages ? r->pt[k].phys : NULL;
        uint64_t ch = phys_chunk(), ci = ph ? r->pt[k].off / ch : 0;
        static _Thread_local int retried;   // a chunk that did not fit resolves as unmapped, once
        if (ph && !ph->chunks[ci].buf && phys_lazy() && !retried) {   // first GPU use of the chunk: back it, look again
            plat_mutex_unlock_shared(&pool_lock);
            phys_back(ph, (uint32_t)ci, (uint32_t)ci);
            retried = 1;
            int ok2 = resolve_locked(addr, size, out);
            retried = 0;
            return ok2;
        }
        if (ph && ph->chunks[ci].buf) {   // contiguous pages of the same chunk
            uint64_t base = ci * ch, cend = base + chunk_len(ph, (uint32_t)ci);
            uint64_t off = r->pt[k].off + rel % NVN_VPOOL_PAGE, end = r->pt[k].off + NVN_VPOOL_PAGE;
            for (uint64_t j = k + 1; j < r->npages && end < cend && r->pt[j].phys == ph && r->pt[j].off == end; j++)
                end += NVN_VPOOL_PAGE;
            out->buffer = ph->chunks[ci].buf;
            out->offset = out->alloc_offset = off - base;
            out->avail = end - off;
            out->alloc = ph->chunks[ci].alloc;
            if (vk_guard_on()) vk_guard_note(&ph->use), vk_guard_note(&ph->chunks[ci].use);
            out->cls = VK_MCLS_DEVICE;
            ok = out->avail >= size;
            if (ok && !ph->used) plat_xchg32(&ph->used, 1);   // read first: the flag is only ever set
        }
    }
    if (ok && !r->used) plat_xchg32(&r->used, 1);
    if (!r)
        for (int i = 0; i < GONE_N; i++)
            if (addr >= gone[i].lo && addr < gone[i].hi) {
                if (vk_guard_on() && vk_guard_use())
                    rt_fatal("vk: MKW_GPU_GUARD: %#llx+%#llx resolved in a pool finalized with this submission still "
                             "queued", (unsigned long long)addr, (unsigned long long)size);
                if (plat_inc32(&n_gone_hits) <= 8)
                    rt_log("vk: %#llx+%#llx resolved in a pool finalized with the replay still queued: nothing bound",
                           (unsigned long long)addr, (unsigned long long)size);
                break;
            }
    plat_mutex_unlock_shared(&pool_lock);
    if (!ok) memset(out, 0, sizeof *out);
    return ok;
}

int vk_mem_virt_phys(uint64_t addr, uint64_t *phys) {
    *phys = 0;
    plat_mutex_lock_shared(&pool_lock);
    const pidx_ent *e = pidx_find(&pool_index, (uintptr_t)addr, 1);
    const PoolRec *r = e ? e->buf : NULL;
    if (r && r->kind == POOL_VIRT) {
        uint64_t k = (addr - r->lo) / NVN_VPOOL_PAGE;
        if (k < r->npages && r->pt[k].phys) *phys = r->pt[k].phys->lo + r->pt[k].off + addr % NVN_VPOOL_PAGE;
    }
    plat_mutex_unlock_shared(&pool_lock);
    return *phys != 0;
}

uint64_t vk_mem_gen(void) {
    return __atomic_load_n(&pool_gen, __ATOMIC_ACQUIRE) + __atomic_load_n(&mirror_gen, __ATOMIC_ACQUIRE);
}

void vk_mem_stats(VkMemStats *s) {
    memset(s, 0, sizeof *s);
    plat_mutex_lock_shared(&pool_lock);
    for (int i = 0; i < pool_index.n; i++) {
        const PoolRec *r = pool_index.e[i].buf;
        if (r->kind == POOL_IMPORT) s->imported_pools++;
        else if (r->kind == POOL_PHYS) s->physical_pools++;
        else s->virtual_pools++;
    }
    plat_mutex_unlock_shared(&pool_lock);
    s->import_failures = (uint64_t)n_import_failures;
    s->upload_batches = upload_batches();
}

void vk_mem_shutdown(void) {
    VkCtx *c = vk_ctx();
    if (!c) return;
    vk_upload_flush(1);
    vk_queue_lock();
    vkDeviceWaitIdle(c->device);
    vk_queue_unlock();
    vk_texture_shutdown();
    upload_shutdown(c);
    plat_mutex_lock(&pool_lock);
    for (int i = 0; i < pool_index.n; i++) destroy_rec(c, pool_index.e[i].buf);
    pool_index.n = 0;
    shadow_reset();
    mirror_reset(c);
    __atomic_add_fetch(&pool_gen, 1, __ATOMIC_RELEASE);
    nphys = 0;
    plat_mutex_unlock(&pool_lock);
    gen_reset();
}
