#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "nvn_api.h"
#include "nvn_hmap.h"
#include "nvn_tex.h"
#include "tex.h"
#include "vk.h"
#include "vk_exec.h"
#include "vk_tex_internal.h"
#include "platform/platform.h"

// ---------------------------------------------------------------- released entries, eviction and the owned list
// Out of memory with no released entry left: live textures that mirror CPU-visible storage (uploaded again from it
// on their next use), that nothing looked up in the last EVICT_IDLE submissions and the GPU did not write, go oldest
// use first. By the draw bindings' check alone (checked), a texture the replay used only as a copy source
// (vk_texture_get) looked idle while a part being recorded could name it: the likely cause of a view destroyed with
// that part not yet sent (MKW_GPU_GUARD, tsvp1 frame 1 477 and tsvp2, 08/11/2026, both after evictions). The race's downscaled render targets (bloom, the final composite's inputs) found the heap full of decoded
// ASTC textures and got no image: a black race (run-race4, 31/10/2026).
#define EVICT_IDLE 64
static uint64_t n_evicted, evicted_bytes;
VkResult evict_idle(VkCtx *c, const VkImageCreateInfo *ci, const VmaAllocationCreateInfo *ai, VkImage *img,
                    VmaAllocation *alloc) {
    VkResult r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    for (int round = 0; r == VK_ERROR_OUT_OF_DEVICE_MEMORY && round < 4; round++) {
        Tex *cand[64];
        int n = 0;
        for (uint32_t i = 0; i < by_key.cap && n < 64; i++) {
            Tex *e = by_key.vals ? by_key.vals[i] : NULL;
            if (e && e->alloc && e->pub.mirrored && !e->pub.gpu_owned && !e->gpu_dirty && !e->released &&
                (uint32_t)(submit_epoch - e->seen) > EVICT_IDLE)
                cand[n++] = e;
        }
        if (!n) break;
        for (int i = 0; i < n; i++) {
            VmaAllocationInfo vi = {0};
            vmaGetAllocationInfo(c->vma, cand[i]->alloc, &vi);
            n_evicted++, evicted_bytes += vi.size;
            destroy_idle(cand[i]);
        }
        vk_mem_collect();
        r = vma_image(c, ci, ai, img, alloc);
    }
    if (n_evicted && !(n_evicted & (n_evicted - 1)))
        rt_log("vk: %llu idle mirrored textures evicted for room (%.0f MiB)", (unsigned long long)n_evicted,
               evicted_bytes / 1048576.0);
    return r;
}

// vk_staging out of memory: released entries, oldest first (as create_tex), then idle mirrored textures (evict_idle's
// rule), worth at least bytes, when the texture lock is free or this thread's (a thread holding it may wait for the
// upload lock the caller holds). 1 if any went. tt4 (09/11/2026) died on a staging buffer with 1 249 released entries
// holding 946 MiB: the idle ones alone were looked at.
int vk_texture_trim(uint64_t bytes) {
    VkCtx *c = vk_ctx();
    if (!c || !plat_rmutex_trylock(&tex_lock)) return 0;
    uint64_t freed = 0;
    int dropped = 0;
    while (rel_head && freed < bytes) {
        VmaAllocationInfo vi = {0};
        if (rel_head->alloc) vmaGetAllocationInfo(c->vma, rel_head->alloc, &vi);
        freed += vi.size, dropped++;
        destroy_idle(rel_head);
    }
    for (uint32_t i = 0; i < by_key.cap && freed < bytes; i++) {
        Tex *e = by_key.vals ? by_key.vals[i] : NULL;
        if (!e || !e->alloc || !e->pub.mirrored || e->pub.gpu_owned || e->gpu_dirty || e->released ||
            (uint32_t)(submit_epoch - e->seen) <= EVICT_IDLE)
            continue;
        VmaAllocationInfo vi = {0};
        vmaGetAllocationInfo(c->vma, e->alloc, &vi);
        freed += vi.size, n_evicted++, evicted_bytes += vi.size;
        destroy_idle(e);
        i--;   // the table shifted an entry into this slot
    }
    unlock();
    return freed > 0 || dropped;
}

int owned_lower(uint64_t a) {
    int lo = 0, hi = nowned;
    while (lo < hi) {
        int m = (lo + hi) / 2;
        if (owned[m]->pub.addr < a) lo = m + 1;
        else hi = m;
    }
    return lo;
}

void own_add(Tex *e) {
    if (e->listed) return;
    plat_mutex_lock(&owned_lock);
    if (nowned == cowned) {
        cowned = cowned ? 2 * cowned : 256;
        if (!(owned = realloc(owned, sizeof *owned * (size_t)cowned))) rt_fatal("vk: out of memory for the owned-texture list");
    }
    int i = owned_lower(e->pub.addr);
    memmove(owned + i + 1, owned + i, sizeof *owned * (size_t)(nowned - i));
    owned[i] = e;
    __atomic_store_n(&nowned, nowned + 1, __ATOMIC_RELEASE);
    if (e->t.L.size > max_owned) max_owned = e->t.L.size;
    e->listed = 1;
    plat_mutex_unlock(&owned_lock);
}

static void own_remove(Tex *e) {
    if (!e->listed) return;
    plat_mutex_lock(&owned_lock);
    for (int i = owned_lower(e->pub.addr); i < nowned && owned[i]->pub.addr == e->pub.addr; i++)
        if (owned[i] == e) {
            memmove(owned + i, owned + i + 1, sizeof *owned * (size_t)(nowned - i - 1));
            __atomic_store_n(&nowned, nowned - 1, __ATOMIC_RELEASE);
            break;
        }
    e->listed = 0;
    plat_mutex_unlock(&owned_lock);
}

void rel_append(Tex *e) {
    VkCtx *c = vk_ctx();
    VmaAllocationInfo vi = {0};
    if (e->alloc && c) vmaGetAllocationInfo(c->vma, e->alloc, &vi);
    e->rel_bytes = vi.size, released_bytes += vi.size;
    e->rel_next = NULL;
    e->rel_prev = rel_tail;
    if (rel_tail) rel_tail->rel_next = e;
    else rel_head = e;
    rel_tail = e;
}

void rel_unlink(Tex *e) {
    released_bytes -= e->rel_bytes, e->rel_bytes = 0;
    if (e->rel_prev) e->rel_prev->rel_next = e->rel_next;
    else rel_head = e->rel_next;
    if (e->rel_next) e->rel_next->rel_prev = e->rel_prev;
    else rel_tail = e->rel_prev;
    e->rel_prev = e->rel_next = NULL;
}

void destroy_tex(Tex *e) {
    own_remove(e);
    tex_gen_bump(e->key.addr);
    nvn_hm_del(&by_key, &e->key);
    Tex *head = nvn_hm_get(&by_addr, &e->key.addr);
    if (head == e) {
        if (e->next_at) nvn_hm_put(&by_addr, &e->key.addr, e->next_at);
        else nvn_hm_del(&by_addr, &e->key.addr);
    } else {
        for (Tex *p = head; p; p = p->next_at)
            if (p->next_at == e) { p->next_at = e->next_at; break; }
    }
    if (e->released) nreleased--, rel_unlink(e);
    vk_mem_defer_use(e->use);   // MKW_GPU_GUARD: the image and its views
    vk_exec_image_gone(e->pub.image);
    // A sparse image also waits for its binds: they are queue operations the parts sent do not cover (vk_sparse.c).
    uint64_t binds = e->sparse ? vk_sparse_destroy(e->pub.image) : 0, prev = 0;
    if (binds) prev = vk_mem_defer_fence(0), vk_mem_defer_fence(prev > binds ? prev : binds);
    vk_defer_image(e->pub.image, e->alloc);
    if (binds) vk_mem_defer_fence(prev);
    vk_mem_defer_use(0);
    free(e);
}

// A released entry is not named by the submission being recorded (taking one back clears released), nor an idle one:
// they go once the parts already sent are done (vk_mem_defer_idle).
// Not when the submission being recorded looked it up (seen): a released entry replaced at a new descriptor of its
// address (entry_of) was still named by the part being recorded, and its view went with the parts already sent only
// (MKW_GPU_GUARD stack, gstack1 08/11/2026). Then it waits for that submission (the replay records one at a time).
void destroy_idle(Tex *e) {
    if (e->seen == submit_epoch) {
        vk_mem_defer_fence(vk_sync_issued() + 1);
        destroy_tex(e);
        vk_mem_defer_fence(0);
        return;
    }
    vk_mem_defer_idle(1);
    destroy_tex(e);
    vk_mem_defer_idle(0);
}

// Released entries with another descriptor at the same address are replaced; past RELEASED_MAX released entries the
// oldest one goes at each release (it comes back on demand, a mirrored one re-uploaded from its storage). TotK
// finalizes and initializes transient textures every frame, so a released entry is mostly taken back within frames.
// Dropping all of them at once when the count passed 4096 destroyed thousands of images in one frame: 333-422 ms
// frames every 40-50 s on a GTX 1050 Ti, then the textures still in use were created and uploaded again.
// The count alone let them hold 1 603 MiB on an 8 GB RTX 4070 Laptop (2 032 entries, at the character select after the
// title's menus, 12/11/2026): with the rest, 7 333 MiB against the driver's 7 180 MiB budget, so Windows moved memory
// out of the GPU and the game ran at 0.24x (the GPU 80-444 ms a frame at 33 W); the ctl "trim" of them brought it back
// to 1.00x at once, under the budget. Their memory is bounded too: MKW_TEX_RELEASED_MIB (default a twelfth of the
// device-local budget at start, 256 MiB to 2 GiB: ~600 MiB on 8 GB, 2 GiB from 24 GB), the oldest recreatable ones
// going first.
#define RELEASED_MAX 2048
static uint64_t released_cap(void) {
    static uint64_t cap;
    if (cap) return cap;
    const char *e = cfg_get("MKW_TEX_RELEASED_MIB");
    uint64_t used, budget;
    int exact;
    if (e && *e) cap = (uint64_t)atoll(e) << 20;
    else if (vk_mem_vram(&used, &budget, &exact) && budget) cap = budget / 12;
    else cap = 512ull << 20;
    if (!(e && *e)) cap = cap < (256ull << 20) ? 256ull << 20 : cap > (2048ull << 20) ? 2048ull << 20 : cap;
    if (!cap) cap = 1;   // MKW_TEX_RELEASED_MIB=0: none kept
    rt_log("vk: released textures keep at most %d entries and %llu MiB (MKW_TEX_RELEASED_MIB)", RELEASED_MAX,
           (unsigned long long)(cap >> 20));
    return cap;
}

// Past the count the oldest goes, as before. Past the memory only entries whose contents come back with them go: a
// mirror of CPU-visible storage the GPU never wrote, re-uploaded on demand (evict_idle's rule). A render target or
// another GPU-written image taken back at the same address would come back without what was drawn there, which the
// console keeps (the same memory); the count's rule takes that risk only past 2 048 entries. On the 8 GB laptop the
// recreatable ones were enough: the cap was never passed in two cups (no "over the cap" line).
static int recreatable(const Tex *e) { return e->pub.mirrored && !e->pub.gpu_owned && !e->gpu_dirty; }

// A released GPU-written entry is kept for what was drawn in it, which the console keeps in the same memory. But only
// until another image is written over that memory: then the console holds the newer contents there, and the entry has
// nothing of its own left to come back with. 1.8.0 makes its shadow map again at each scene, at a new place in the
// render-buffer pool 0x24000 bytes past the last (ph4, 16/11/2026: 4096x4096 D16, 96 MiB each, 61 addresses in 4
// minutes of the title's demo runs): every one overlaps the next, all were kept as GPU-written, and the released ones
// held 9.9 GB until the device ran out of memory (amdgpu: "Not enough memory for command submission", then device
// lost); with the console's map, 3.3 GB after 50 s of a two-player race (sp1). Those whose memory a later write
// (a newer wr_fence; an equal one is not counted) covers in part go, oldest first and only until the cap holds: the
// game takes many back at the same key, and each one dropped is made again (the first version dropped every one it
// found: 2 522 shadow maps made in rel1 against 204, the turbo's speed 1.67x against 1.90x). Writers larger than
// WRITER_SPAN are left out of the search (the window it scans before each entry): they are textures, not render
// targets.
#define WRITER_SPAN (512ull << 20)
typedef struct { uint64_t lo, hi, fence; } Span;
static int by_lo(const void *a, const void *b) {
    uint64_t x = ((const Span *)a)->lo, y = ((const Span *)b)->lo;
    return (x > y) - (x < y);
}
static uint64_t overwritten_n, overwritten_bytes;
static void drop_overwritten(void) {
    Span *w = malloc(sizeof *w * (size_t)(by_key.n + 1));
    Tex **gone = malloc(sizeof *gone * (size_t)(nreleased + 1));
    if (!w || !gone) { free(w), free(gone); return; }
    int nw = 0, ng = 0;
    for (uint32_t i = 0; i < by_key.cap; i++) {
        Tex *e = by_key.vals[i];
        if (e && e->wr_fence && e->t.L.size && e->t.L.size <= WRITER_SPAN)
            w[nw++] = (Span){e->key.addr, e->key.addr + e->t.L.size, e->wr_fence};
    }
    qsort(w, (size_t)nw, sizeof *w, by_lo);
    uint64_t over = released_bytes - released_cap(), freed = 0;   // the caller: over the cap
    for (Tex *e = rel_head; e && freed < over; e = e->rel_next) {
        if (recreatable(e) || e->sparse || !e->wr_fence || !e->t.L.size) continue;
        uint64_t lo = e->key.addr, hi = lo + e->t.L.size, from = lo > WRITER_SPAN ? lo - WRITER_SPAN : 0;
        int a = 0, b = nw;   // the first writer starting at from or later
        while (a < b) {
            int m = (a + b) / 2;
            if (w[m].lo < from) a = m + 1;
            else b = m;
        }
        for (int k = a; k < nw && w[k].lo < hi; k++)
            if (w[k].hi > lo && w[k].fence > e->wr_fence) { gone[ng++] = e, freed += e->rel_bytes; break; }
    }
    for (int i = 0; i < ng; i++) overwritten_n++, overwritten_bytes += gone[i]->rel_bytes, destroy_idle(gone[i]);
    free(w), free(gone);
}

void sweep_released(void) {
    while (nreleased > RELEASED_MAX && rel_head) destroy_idle(rel_head);
    if (released_bytes <= released_cap()) return;
    for (Tex *e = rel_head, *next; e && released_bytes > released_cap(); e = next) {
        next = e->rel_next;
        if (recreatable(e)) destroy_idle(e);
    }
    // The search walks every entry: only once the kept ones grew 64 MiB since the last one (a release at a time
    // otherwise, on the replay thread).
    // MKW_TEX_OVERWRITTEN=0 (a measurement): kept as before, for comparing a run with and without.
    static uint64_t searched_at;
    static int on = -1;
    if (on < 0) on = rt_knob("TEX_OVERWRITTEN", "1") != 0;
    if (on && released_bytes > released_cap() && released_bytes >= searched_at + (64ull << 20)) {
        uint64_t n0 = overwritten_n, b0 = overwritten_bytes;
        drop_overwritten();
        searched_at = released_bytes;
        static int logs;            // the first 8, then at each GiB more in all (ab1: ~4 000 dropped in 7 minutes)
        static uint64_t logged_gib;
        if (overwritten_n > n0 && (logs++ < 8 || (overwritten_bytes >> 30) > logged_gib))
            rt_log("vk: released textures written over by a later image: %llu dropped (%llu MiB), %llu in all (%llu MiB)",
                   (unsigned long long)(overwritten_n - n0), (unsigned long long)((overwritten_bytes - b0) >> 20),
                   (unsigned long long)overwritten_n, (unsigned long long)(overwritten_bytes >> 20)),
            logged_gib = overwritten_bytes >> 30;
    }
    static uint64_t logged_at;   // what stays over the cap: released images the GPU wrote (render targets and such)
    if (released_bytes > released_cap() && released_bytes > logged_at + (256ull << 20)) {
        logged_at = released_bytes;
        rt_log("vk: released textures hold %llu MiB, over the %llu MiB cap: the rest are GPU-written, kept",
               (unsigned long long)(released_bytes >> 20), (unsigned long long)(released_cap() >> 20));
    }
}

