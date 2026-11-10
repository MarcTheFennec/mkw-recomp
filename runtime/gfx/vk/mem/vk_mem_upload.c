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
#include "platform/platform.h"

// ---------------------------------------------------------------- upload batches and deferred destruction
typedef struct { VkBuffer buf; VmaAllocation alloc; } Staging;
typedef struct {
    VkCommandBuffer cb;
    VkFence fence;
    uint64_t seq;
    Staging *stg;
    int nstg, cap;
    VkDeviceSize stg_bytes;   // staging the batch holds (vk_upload_flush_over)
    uint64_t texels;          // ASTC texels its dispatches decode (vk_upload_work)
    uint32_t calls;           // vk_upload_begin calls that recorded into it
    int ut;                   // MKW_UPLOAD_TIME slot, -1: untimed
    uint32_t crumb;           // its start mark (diag/vk_crumbs.c), 0 none
} Batch;

enum { DEAD_IMAGE = 1, DEAD_IMAGE_VMA, DEAD_VIEW, DEAD_BUFFER_VMA, DEAD_CALL };
// seq: the upload batch, tl: the submission part (vk_sync_part_done) and fence: the submission (timeline value) that may
// still use it; vma: NULL = the runtime's; use: MKW_GPU_GUARD, the newest part that named it (vk_mem_defer_use)
typedef struct { int kind; uint64_t handle; VmaAllocation alloc; uint64_t seq, tl, fence; VmaAllocator vma; uint64_t use; } Dead;

static PlatRMutex cmd_lock;
static int depth;                     // vk_upload_begin calls not yet ended: a flush inside one would end its cb
static PlatOnce cmd_once = PLAT_ONCE_INIT;
static VkCommandPool cmd_pool;
static Batch open_b;
static int open_active;
static Batch *inflight;
static int ninflight, cap_inflight;
static Batch *spare;                  // completed batches whose command buffer and fence are reused
static int nspare, cap_spare;
static uint64_t next_seq, done_seq;   // batches handed out / completed (all batches up to done_seq)
static Dead *dead;
static int ndead, cap_dead;
static uint64_t tl_noted;             // newest part value vk_sync.c reported submitted

static void cmd_init(void *p) {
    (void)p;
    return;
}

void vk_mem_grow(void **arr, int *cap, size_t elem) {
    *cap = *cap ? *cap * 2 : 16;
    void *n = realloc(*arr, (size_t)*cap * elem);
    if (!n) rt_fatal("vk: out of memory (upload bookkeeping)");
    *arr = n;
}

static void free_staging(VkCtx *c, Batch *b) {
    for (int i = 0; i < b->nstg; i++) vmaDestroyBuffer(c->vma, b->stg[i].buf, b->stg[i].alloc);
    b->nstg = 0;
}

// Batches complete in submission order (one queue): retire from the front while their fences are signalled.
static void retire(VkCtx *c, int wait_all) {
    int k = 0;
    for (; k < ninflight; k++) {
        Batch *b = &inflight[k];
        VkResult r = wait_all ? vkWaitForFences(c->device, 1, &b->fence, VK_TRUE, UINT64_MAX)
                              : vkGetFenceStatus(c->device, b->fence);
        if (r == VK_ERROR_DEVICE_LOST) {
            vk_exec_recent_log();
            rt_fatal("vk: upload fence failed (VK_ERROR_DEVICE_LOST)");
        }
        if (r != VK_SUCCESS) break;
        free_staging(c, b);
        vk_uptime_done(b->ut, b->seq, b->stg_bytes, b->texels, b->calls);
        done_seq = b->seq;
        if (nspare == cap_spare) vk_mem_grow((void **)&spare, &cap_spare, sizeof *spare);
        spare[nspare++] = *b;
    }
    memmove(inflight, inflight + k, (size_t)(ninflight - k) * sizeof *inflight);
    ninflight -= k;
}

static void collect_locked(VkCtx *c) {
    retire(c, 0);
    uint64_t tl = vk_sync_part_done(), fdone = 0;
    if (c->timeline) vkGetSemaphoreCounterValue(c->device, c->timeline, &fdone);
    int k = 0;
    for (int i = 0; i < ndead; i++) {
        Dead *d = &dead[i];
        if (d->seq > done_seq || d->tl > tl || d->fence > fdone) { dead[k++] = *d; continue; }
        if (d->use && d->kind != DEAD_CALL)
            vk_guard_dead(d->kind == DEAD_VIEW ? "image view" : d->kind == DEAD_BUFFER_VMA ? "buffer" : "image",
                          d->handle, 0, d->use);
        switch (d->kind) {
        case DEAD_IMAGE: vkDestroyImage(c->device, (VkImage)(uintptr_t)d->handle, NULL); break;
        case DEAD_IMAGE_VMA: vmaDestroyImage(c->vma, (VkImage)(uintptr_t)d->handle, d->alloc); break;
        case DEAD_VIEW: vkDestroyImageView(c->device, (VkImageView)(uintptr_t)d->handle, NULL); break;
        case DEAD_BUFFER_VMA: vmaDestroyBuffer(d->vma ? d->vma : c->vma, (VkBuffer)(uintptr_t)d->handle, d->alloc); break;
        case DEAD_CALL: ((void (*)(void *))(uintptr_t)d->handle)((void *)d->alloc); break;
        }
    }
    ndead = k;
}

VkCommandBuffer vk_upload_begin(void) {
    plat_once(&cmd_once, cmd_init, NULL);
    plat_rmutex_lock(&cmd_lock);
    VkCtx *c = vk_ctx();
    if (!c) rt_fatal("vk: upload before vk_init");
    depth++;
    if (open_active) {
        open_b.calls++;
        vk_uptime_call(open_b.ut, __builtin_return_address(0));
        return open_b.cb;
    }
    if (!cmd_pool) {
        VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                      .queueFamilyIndex = c->gfx_family};
        if (vkCreateCommandPool(c->device, &pi, NULL, &cmd_pool) != VK_SUCCESS) rt_fatal("vk: upload command pool");
    }
    retire(c, 0);
    memset(&open_b, 0, sizeof open_b);
    if (nspare) {
        Batch s = spare[--nspare];
        open_b.cb = s.cb, open_b.fence = s.fence, open_b.stg = s.stg, open_b.cap = s.cap;
        vkResetCommandBuffer(open_b.cb, 0);
        vkResetFences(c->device, 1, &open_b.fence);
    } else {
        VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cmd_pool,
                                          .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vkAllocateCommandBuffers(c->device, &ai, &open_b.cb) != VK_SUCCESS ||
            vkCreateFence(c->device, &fi, NULL, &open_b.fence) != VK_SUCCESS)
            rt_fatal("vk: upload command buffer");
    }
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(open_b.cb, &bi);
    open_b.seq = ++next_seq;
    open_b.crumb = vk_crumb_begin(open_b.cb, VK_CRUMB_UPLOAD, (uint32_t)open_b.seq, 0, 0, 0, NULL, 0);
    open_b.calls = 1;
    open_b.ut = vk_uptime_begin(open_b.cb);
    vk_uptime_call(open_b.ut, __builtin_return_address(0));
    open_active = 1;
    return open_b.cb;
}

void vk_upload_end(void) {
    depth--;
    plat_rmutex_unlock(&cmd_lock);
}

void vk_upload_work(uint64_t texels) {
    plat_rmutex_lock(&cmd_lock);
    if (open_active) open_b.texels += texels;
    plat_rmutex_unlock(&cmd_lock);
}

// Staging memory comes from a pool of its own that keeps STG_MIN_BLOCKS blocks from its first use (at boot). In the
// default pools VMA creates a block under the lock of its memory type (VmaBlockVector::Allocate), so a block made by
// the headroom thread (vk_headroom.c) or a burst of uploads held every staging allocation of the render thread: one
// took 47-50 ms, for a few bytes, in the first frame after a save load. Larger buffers than a block go to the default
// pool.
#define STG_BLOCK (64ull << 20)
#define STG_MIN_BLOCKS 2
static VmaPool stg_pool;
static int stg_pool_tried;

// Waits for the GPU up to the newest submission, its parts already sent (the one being recorded) and every upload batch
// in flight, then frees what they held.
static void wait_and_collect(VkCtx *c) {
    vk_sync_fence_wait(vk_sync_issued(), UINT64_MAX);
    plat_rmutex_lock(&cmd_lock);
    uint64_t parts = tl_noted;
    plat_rmutex_unlock(&cmd_lock);
    vk_sync_part_wait(parts);
    plat_rmutex_lock(&cmd_lock);
    retire(c, 1);
    collect_locked(c);
    plat_rmutex_unlock(&cmd_lock);
}

// NULL when out of memory and fatal is 0.
static void *staging(VkDeviceSize size, VkBuffer *buf, VmaAllocation *alloc, int fatal) {
    VkCtx *c = vk_ctx();
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size ? size : 16,
                             .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo aci = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                   .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    if (!__atomic_load_n(&stg_pool_tried, __ATOMIC_ACQUIRE)) {
        plat_once(&cmd_once, cmd_init, NULL);
        plat_rmutex_lock(&cmd_lock);
        if (!stg_pool_tried) {
            uint32_t type;
            VmaPoolCreateInfo pi = {.blockSize = STG_BLOCK, .minBlockCount = STG_MIN_BLOCKS};
            if (vmaFindMemoryTypeIndexForBufferInfo(c->vma, &bi, &aci, &type) == VK_SUCCESS) {
                pi.memoryTypeIndex = type;
                if (vmaCreatePool(c->vma, &pi, &stg_pool) != VK_SUCCESS) stg_pool = NULL;
            }
            rt_log("vk: staging pool %s (%d blocks of %llu MiB kept)", stg_pool ? "created" : "unavailable: default pool",
                   STG_MIN_BLOCKS, (unsigned long long)(STG_BLOCK >> 20));
            __atomic_store_n(&stg_pool_tried, 1, __ATOMIC_RELEASE);
        }
        plat_rmutex_unlock(&cmd_lock);
    }
    if (stg_pool && bi.size <= STG_BLOCK / 2) aci.pool = stg_pool;
    VmaAllocationInfo info;
    VkBuffer b;
    VmaAllocation a;
    VkResult r = vmaCreateBuffer(c->vma, &bi, &aci, &b, &a, &info);
    // Out of memory: the default pool (the staging pool's blocks may all be in use), then idle textures given back
    // (run-race6 stopped on a 341 KiB staging buffer with the heap full of them).
    if (r != VK_SUCCESS && aci.pool) aci.pool = NULL, r = vmaCreateBuffer(c->vma, &bi, &aci, &b, &a, &info);
    for (int round = 0; r != VK_SUCCESS && round < 4 && vk_texture_trim(bi.size * 2 + (8ull << 20)); round++) {
        vk_mem_collect();
        r = vmaCreateBuffer(c->vma, &bi, &aci, &b, &a, &info);
    }
    // Last: wait for the GPU up to the newest submission, its parts already sent (the one being recorded) and every
    // upload batch in flight, so finalized pools, released textures waiting for their fence (cap-race1 died with 454
    // of them and heap0 at its cap; img-par4 with 2 630) and the staging of finished batches are freed, then try once
    // more.
    if (r != VK_SUCCESS && (retiring_count() || dead_pending() || ninflight)) {
        wait_and_collect(c);
        r = vmaCreateBuffer(c->vma, &bi, &aci, &b, &a, &info);
        rt_log("vk: staging of %llu bytes after waiting for the GPU and the upload batches: %s",
               (unsigned long long)size, r == VK_SUCCESS ? "made" : "still out of memory");
    }
    // No retry in device-local host-visible memory: outside a pool VMA already tries every memory type that fits once
    // the preferred one fails (heap1's host-visible types on the Radeon 660M; h0b, 07/11/2026: heap0 at 200 MiB
    // replays the race without failing). cap-race1, cap-race6 and img-par4 died with both heaps at their caps.
    if (r != VK_SUCCESS) {
        vram_log(0);
        if (!fatal) return NULL;
        rt_fatal("vk: staging buffer of %llu bytes failed (%d)", (unsigned long long)size, (int)r);
    }
    *buf = b;
    if (alloc) *alloc = a;   // the caller destroys it (vmaDestroyBuffer) after vk_upload_flush(1)
    else {
        if (open_b.nstg == open_b.cap) vk_mem_grow((void **)&open_b.stg, &open_b.cap, sizeof *open_b.stg);
        open_b.stg[open_b.nstg++] = (Staging){b, a};
        open_b.stg_bytes += size;
    }
    return info.pMappedData;
}

void *vk_staging(VkDeviceSize size, VkBuffer *buf, VmaAllocation *alloc) { return staging(size, buf, alloc, 1); }

// Out of memory: released and idle textures go (vk_texture_trim), and, with no vk_upload_begin open (this thread's or
// another's: the lock waits for it), the open batch is sent and waited for; then the GPU and the batches in flight are
// waited for and what they held is freed. What was released or evicted while the batch was open waits for it (defer),
// and nothing inside the batch can send it (tt4, 09/11/2026: 978 releases still pending after waiting for the GPU).
void vk_mem_relieve(uint64_t bytes, const char *what) {
    static volatile int32_t n_logged;
    VkCtx *c = vk_ctx();
    if (!c) return;
    uint64_t t0 = plat_now_ns();
    vk_texture_trim(bytes);
    plat_once(&cmd_once, cmd_init, NULL);
    plat_rmutex_lock(&cmd_lock);
    int outside = !depth;
    if (outside) vk_upload_flush(1);
    plat_rmutex_unlock(&cmd_lock);
    wait_and_collect(c);
    if (plat_inc32(&n_logged) <= 32)
        rt_log("vk: out of memory for %s of %llu bytes: textures trimmed, %s, GPU waited for (%.1f ms)", what,
               (unsigned long long)bytes, outside ? "open upload batch sent" : "inside an upload batch",
               (plat_now_ns() - t0) / 1e6);
}

// vk_upload_begin and the caller's staging buffer, before it records anything; out of memory with no other
// vk_upload_begin of this thread open, vk_mem_relieve outside the batch and both are tried again.
VkCommandBuffer vk_upload_begin_staging(VkDeviceSize size, VkBuffer *buf, void **map) {
    VkCommandBuffer cb = vk_upload_begin();
    if (depth > 1) {
        *map = vk_staging(size, buf, NULL);
        return cb;
    }
    if ((*map = staging(size, buf, NULL, 0))) return cb;
    vk_upload_end();
    vk_mem_relieve(size * 2 + (8ull << 20), "staging");
    cb = vk_upload_begin();
    *map = vk_staging(size, buf, NULL);
    return cb;
}

// A caller that adds to the batch without waiting (vk_tex_texel.c writes, vk_tex_upload.c) sends it once it holds
// `limit` bytes of staging, UPLOAD_TEXELS ASTC texels to decode or UPLOAD_CALLS recordings: the batch is only sent with
// the next submission, and the title capture's 8 155 writes in one batch ran the staging out of memory (wt1). The other
// two bound its GPU time under amdgpu's 2 s job timeout, which the race capture's load passed (xb-*, hang1): its
// batches took 170-430 ms per ~67 M texels, and 752-1 112 ms with 642 textures (MKW_UPLOAD_TIME up1, up2; each a full
// barrier, the GPU-only ones a clear of the whole image). Not inside a vk_upload_begin.
#define UPLOAD_TEXELS (64ull << 20)
#define UPLOAD_CALLS 128
void vk_upload_flush_over(VkDeviceSize limit) {
    plat_once(&cmd_once, cmd_init, NULL);
    plat_rmutex_lock(&cmd_lock);
    int over = open_active && !depth &&
               (open_b.stg_bytes >= limit || open_b.texels >= UPLOAD_TEXELS || open_b.calls >= UPLOAD_CALLS);
    plat_rmutex_unlock(&cmd_lock);
    if (over) vk_upload_flush(0);
}

uint64_t vk_upload_flush(int wait) {
    plat_once(&cmd_once, cmd_init, NULL);
    plat_rmutex_lock(&cmd_lock);
    VkCtx *c = vk_ctx();
    uint64_t seq = next_seq;
    if (c && open_active) {
        for (int i = 0; i < open_b.nstg; i++) vmaFlushAllocation(c->vma, open_b.stg[i].alloc, 0, VK_WHOLE_SIZE);
        vk_uptime_end(open_b.cb, open_b.ut);
        vk_crumb_end(open_b.cb, VK_CRUMB_UPLOAD, open_b.crumb);
        vkEndCommandBuffer(open_b.cb);
        VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &open_b.cb};
        vk_queue_lock();
        VkSparseOrder so = vk_sparse_order(0, 0);   // ordered with the sparse binds like every graphics-queue submission
        VkPipelineStageFlags ws = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkTimelineSemaphoreSubmitInfo tl = {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                                            .waitSemaphoreValueCount = so.wait ? 1u : 0u, .pWaitSemaphoreValues = &so.wait,
                                            .signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &so.signal};
        if (so.ssem) si.pNext = &tl, si.signalSemaphoreCount = 1, si.pSignalSemaphores = &so.ssem;
        if (so.ssem && so.wait) si.waitSemaphoreCount = 1, si.pWaitSemaphores = &so.wsem, si.pWaitDstStageMask = &ws;
        VkResult r = vkQueueSubmit(c->gfx_queue, 1, &si, open_b.fence);
        vk_queue_unlock();
        if (r == VK_ERROR_DEVICE_LOST) vk_exec_recent_log();
        if (r != VK_SUCCESS) rt_fatal("vk: upload submit failed (%d)", (int)r);
        if (ninflight == cap_inflight) vk_mem_grow((void **)&inflight, &cap_inflight, sizeof *inflight);
        inflight[ninflight++] = open_b;
        open_active = 0;
    }
    if (c && wait) {
        while (ninflight && done_seq < seq) {
            VkResult r = vkWaitForFences(c->device, 1, &inflight[0].fence, VK_TRUE, UINT64_MAX);
            if (r == VK_ERROR_DEVICE_LOST) {
                vk_exec_recent_log();
                rt_fatal("vk: upload fence wait failed (VK_ERROR_DEVICE_LOST)");
            }
            retire(c, 0);
        }
    }
    if (c) collect_locked(c);
    plat_rmutex_unlock(&cmd_lock);
    return seq;
}

static _Thread_local VmaAllocator defer_vma;   // vk_defer_buffer_in: the allocator of the buffer being deferred
// What a recording thread destroys may be named by the part it has not sent yet, or by a chunk not sent yet: it waits
// for the whole submission. A texture released or idle in it (vk_mem_defer_idle) waits only for the parts already sent:
// 07/11/2026, tying those to the submission too left the race's load (ht1) out of memory with 852-5 250 of them pending,
// and tying everything to the parts sent hung the GPU once in a capture replay (c-ser1, a page fault in 0x8001...).
static _Thread_local uint64_t rec_fence;
static _Thread_local int idle_defer;
static _Thread_local uint64_t defer_use;
static _Thread_local uint64_t defer_fence;   // vk_mem_defer_fence: the submission the object waits for, whatever the rest

void vk_mem_recording(uint64_t fence) { rec_fence = fence; }
void vk_mem_defer_idle(int on) { idle_defer += on ? 1 : -1; }
void vk_mem_defer_use(uint64_t use) { defer_use = vk_guard_on() ? use : 0; }
uint64_t vk_mem_defer_fence(uint64_t fence) {
    uint64_t prev = defer_fence;
    defer_fence = fence;
    return prev;
}

static void defer(int kind, uint64_t handle, VmaAllocation alloc) {
    plat_once(&cmd_once, cmd_init, NULL);
    plat_rmutex_lock(&cmd_lock);
    // MKW_GPU_GUARD: tied to the submission being recorded, every part of it is covered; otherwise only the parts sent.
    uint64_t fence = defer_fence ? defer_fence : idle_defer ? 0 : rec_fence;
    if (defer_use && !fence)
        vk_guard_tie(kind == DEAD_VIEW ? "image view" : kind == DEAD_BUFFER_VMA ? "buffer" : "image", handle, 0,
                     defer_use, tl_noted);
    if (ndead == cap_dead) vk_mem_grow((void **)&dead, &cap_dead, sizeof *dead);
    // The open batch may record into the object: it counts as the newest user.
    dead[ndead++] = (Dead){kind, handle, alloc, open_active ? open_b.seq : next_seq, tl_noted,
                           fence, defer_vma, defer_use};
    plat_rmutex_unlock(&cmd_lock);
}

int dead_pending(void) {
    plat_once(&cmd_once, cmd_init, NULL);
    plat_rmutex_lock(&cmd_lock);
    int n = ndead;
    plat_rmutex_unlock(&cmd_lock);
    return n;
}

void vk_defer_image(VkImage img, VmaAllocation alloc) {
    if (img) defer(alloc ? DEAD_IMAGE_VMA : DEAD_IMAGE, (uint64_t)(uintptr_t)img, alloc);
}
void vk_defer_view(VkImageView view) { if (view) defer(DEAD_VIEW, (uint64_t)(uintptr_t)view, NULL); }
void vk_defer_buffer(VkBuffer buf, VmaAllocation alloc) {
    if (buf) defer(DEAD_BUFFER_VMA, (uint64_t)(uintptr_t)buf, alloc);
}
// fn(arg) once the upload batches recorded so far are done (a buffer they use goes back to a pool, vk_astc.c).
void vk_defer_call(void (*fn)(void *), void *arg) { defer(DEAD_CALL, (uint64_t)(uintptr_t)fn, (VmaAllocation)arg); }

void vk_defer_buffer_in(VmaAllocator vma, VkBuffer buf, VmaAllocation alloc) {
    defer_vma = vma;
    vk_defer_buffer(buf, alloc);
    defer_vma = NULL;
}

void vk_mem_note_submit(uint64_t part_value) {
    plat_once(&cmd_once, cmd_init, NULL);
    plat_rmutex_lock(&cmd_lock);
    if (part_value > tl_noted) tl_noted = part_value;
    plat_rmutex_unlock(&cmd_lock);
}

void vk_mem_collect(void) {
    VkCtx *c = vk_ctx();
    if (!c) return;
    plat_once(&cmd_once, cmd_init, NULL);
    plat_rmutex_lock(&cmd_lock);
    collect_locked(c);
    plat_rmutex_unlock(&cmd_lock);
}

uint64_t upload_batches(void) { return next_seq; }

// vk_mem_shutdown, the device idle: every batch and dead object goes, and the staging pool.
void upload_shutdown(VkCtx *c) {
    plat_rmutex_lock(&cmd_lock);
    retire(c, 1);
    uint64_t keep = tl_noted;
    tl_noted = 0;
    for (int i = 0; i < ndead; i++) dead[i].seq = dead[i].tl = dead[i].fence = 0;   // the device is idle
    collect_locked(c);
    tl_noted = keep;
    for (int i = 0; i < nspare; i++) {
        vkDestroyFence(c->device, spare[i].fence, NULL);
        free(spare[i].stg);
    }
    nspare = 0;
    if (cmd_pool) vkDestroyCommandPool(c->device, cmd_pool, NULL);
    cmd_pool = VK_NULL_HANDLE;
    if (stg_pool) vmaDestroyPool(c->vma, stg_pool);   // after retire: no staging buffer is left in it
    stg_pool = NULL;
    stg_pool_tried = 0;
    plat_rmutex_unlock(&cmd_lock);
}
