// Sparse textures (NVN_TEXTURE_FLAGS_SPARSE in a virtual pool) as Vulkan sparse images. The game maps 64 KiB pages of
// the texture's storage with nvnMemoryPoolMapVirtual; each mapped page here binds the Vulkan sparse blocks that hold
// the same texels to memory of that physical page.
//
// The storage layout is the one the game maps (fitted with tools/gpu/sparse_fit.py on run-sp1, 31/10/2026): a layer is
// its levels with whole tiles (nvn_sparse_tile_layout), level after level, each in row-major tiles of one page; the
// layer stride is those pages and nothing else. The mip tails (the levels smaller than a tile) were never mapped by
// MKW (D16 512x512x101: 10-page layers at 0xa0000 * k; the three 8192x8192x68 arrays: the 1-page level 4 mapped 68
// times, level 3 4 x 68). They are not in the pages, but the game copies into them (levels 5-8 of the 8192x8192x68
// arrays, m13): they get memory of their own, bound once (tail_alloc).
//
// Memory: one 64 KiB slot per (physical page, Vulkan block inside it, format), so virtual pages mapped to the same
// physical page share their texels, as on the console (the game maps a whole layer to one page as a stand-in). ASTC is
// decoded (BC7 or RGBA8): one ASTC 8x8 page of 512x512 texels is 4 BC7 blocks or 16 RGBA8 blocks, each a slot of its
// own. Slots come zeroed from 16 MiB blocks and are freed once no binding names them and the GPU is past the unbind.
//
// Changed blocks are listed per image and bound by the next graphics-queue submission (vk_sparse_order from
// vk_sync.c and vk_upload_flush): vkQueueBindSparse waits for every earlier submission, parts of the frame being
// replayed and uploads included, and every later submission waits for the bind. Sparse binding has no implicit order
// with command buffers; waiting only for the previous frame's value let a bind run beside the parts already sent of
// the current one (hypothesis for the TCP faults of run-race41 and run-mesh3: amdgpu clears the PTEs of a replaced
// range before writing the new ones, and a read in flight there faults). A slot that does not fit leaves its block
// non-resident (reads give zero: residencyNonResidentStrict), with a log line.
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "nvn_api.h"
#include "nvn_hmap.h"
#include "nvn_tex.h"
#include "vk.h"
#include "vk_defaults.h"
#include "platform/platform.h"

#define PAGE NVN_VPOOL_PAGE
#define BLOCK_SLOTS 256   // 16 MiB of 64 KiB slots
#define SMALL_SPARSE (4u << 20)   // storage of an image that gets slots past MKW_SPARSE_MIB

typedef struct SlotBlock {
    VkBuffer buf;                 // over the whole block, for the zero fill; none when no memory type serves both (#215)
    VmaAllocation alloc;
    VkDeviceMemory mem;
    VkDeviceSize base;            // offset of the block in mem
    VkDeviceSize slot_size;       // bytes of each of its slots
    uint32_t type_bits;           // memory types of the images it serves
    uint32_t used;
    uint32_t top;                 // slots handed out in order to lanes (lane_get); below it, holes are reused
    uint8_t busy[BLOCK_SLOTS];    // 0 never used (zero from the block's fill), 1 in use, 2 freed (its texels stay)
    struct SlotBlock *next;
} SlotBlock;

typedef struct { uint64_t pa; uint32_t sub; int32_t fmt; } SlotKey;
typedef struct Slot {
    SlotKey key;
    SlotBlock *blk;
    uint32_t idx;
    int32_t refs;                 // block bindings that name it
    int32_t reaps;                // frees queued behind a submission (slot_reap)
    int32_t written;              // a copy or a pass wrote into it (vk_sparse_written)
} Slot;

typedef struct SparseImg {
    VkImage img;
    uint64_t addr, size;          // NVN storage range
    VkImageAspectFlags aspect;
    VkFormat fmt;
    uint32_t w, h, layers, nlev;  // nlev: levels bound per tile (whole NVN tiles and below the Vulkan mip tail)
    uint32_t ntw, nth;            // NVN tile in texels
    uint32_t gw, gh;              // Vulkan sparse block in texels
    uint32_t nnvn;                // NVN levels with whole tiles (nnvn >= nlev)
    uint32_t tiles_x[16], tiles_y[16], tile0[16], stride;   // NVN tiles a row, rows, first page of each level; pages a layer
    uint32_t bx[16], blk0[16], nblk;           // Vulkan blocks a row and first block of each level; blocks a layer
    VkDeviceSize slot_size;
    uint32_t type_bits;
    Slot **bound;                 // layers * nblk
    // The levels NVN keeps out of its tiles (below nlev: smaller than a tile, and the Vulkan mip tail) have no pages
    // the game maps, so they never got memory and read as zero, copies into them dropped. They get memory of their
    // own instead (tail_*: one allocation, bound once): the race's bushes and cacti sample those levels of the terrain
    // arrays and came out black (m8, m10: still black with every tile resident).
    VmaAllocation tail_alloc;
    VkDeviceSize va_size;   // the image's memory requirements' size: the address space it reserves
    VkDeviceMemory tail_mem;
    VkDeviceSize tail_base;
    uint32_t nmips, tail_end, tail_blocks;   // levels; first Vulkan mip tail level (nmips: none); blocks a layer in
                                             // levels nlev..tail_end-1
    VkSparseImageMemoryRequirements tail_req;
    int tail_pending;                 // bound by the next flush
    uint32_t tail_written;            // levels outside the tiles a copy or pass wrote (bit per level), for the log
    // The submission whose completion means every bind issued for s has run (vk_sparse_destroy). Binds are queue
    // operations, not parts: a texture destroyed while idle waits only for the parts already sent (vk_tex_evict.c), and
    // NVIDIA's kernel mapper then ran a queued bind of an image already destroyed: "Requested object not found" for
    // pUnmap->hVirtualMemory in mem_mapper.c, and the device lost (620.70.06, RTX 5070 Ti, 09/11/2026).
    uint64_t bind_fence;

    uint8_t *dirty;               // bound changed since the last flush; listed in dl
    uint32_t *dl;
    int ndl, cdl;
    struct SparseImg *next;
} SparseImg;

static PlatMutex sp_lock = PLAT_MUTEX_INIT;
static SparseImg *images;
static nvn_hmap slots = {.klen = sizeof(SlotKey)};
static nvn_hmap cap_pending = {.klen = sizeof(SlotKey)};   // runtime/gfx/cap: SlotKey -> saved texels
static SlotBlock *blocks;
static int ndirty;               // images with a non-empty dirty list
static Slot **dying;              // refs reached 0 since the last flush: reaped behind the next submission
static int ndying, cdying;
static VkSemaphore order_tl;      // timeline: one value per graphics-queue submission and per bind (vk_sparse_order)
static VkSemaphore bind_sem;      // MKW_SPARSE_ORDER=0: binary, signalled by a bind for the next replayed submission
static uint64_t order_val, bind_val;
static int binary_pending;        // MKW_SPARSE_ORDER=0: a bind signalled bind_sem for the submission being made   // last value handed out; value of the last bind (0: none yet)
static uint64_t sparse_va;   // address space the live sparse images reserve (their memory requirements' size)
static int64_t n_slots, n_failed, n_binds, n_images, n_blocks, n_over, n_written, n_tail_writes, n_host_blocks;
static VmaAllocation *dying_tails;   // tail memory of destroyed images: freed behind the next submission
static int ndying_tails, cdying_tails, ntail_pending;

static void grow(void **arr, int *cap, size_t elem) {
    *cap = *cap ? *cap * 2 : 256;
    if (!(*arr = realloc(*arr, (size_t)*cap * elem))) rt_fatal("vk: out of memory (sparse)");
}

// MKW_SPARSE_MIB: most slot memory. run-sp3 (3 GB cap) bound 1 472 MiB, nearly all in three 8192x8192x68 arrays
// (BC6H 509, two ASTC as BC7 505 and 374), and 64 other textures (menu icons, the ASTC the arrays are copied from) had
// no memory left. Past it a block stays non-resident (reads zero); a new image binds its coarse levels first.
// Unset, it comes from the GPU type and its largest device-local heap (vk_default_sparse_mib). 0 lifts it.
static uint64_t budget_default(void) {
    VkCtx *c = vk_ctx();
    if (!c) return vk_default_sparse_mib(0, 0);
    uint64_t vram = 0;
    for (uint32_t h = 0; h < c->mem_props.memoryHeapCount; h++)
        if (c->mem_props.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT && c->mem_props.memoryHeaps[h].size > vram)
            vram = c->mem_props.memoryHeaps[h].size;
    return vk_default_sparse_mib(c->props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, vram >> 20);
}

static uint64_t budget(void) {
    static uint64_t v;
    if (!v) {
        const char *e = cfg_get("MKW_SPARSE_MIB");
        uint64_t mib = e && *e ? (uint64_t)atol(e) : budget_default();
        rt_log("vk: sparse: tile slots up to %llu MiB (MKW_SPARSE_MIB%s; 0 lifts it)", (unsigned long long)mib,
               e && *e ? "" : " unset, from the GPU");
        v = mib ? mib << 20 : UINT64_MAX;
    }
    return v;
}

static int on(void) {
    static int v = -1;
    if (v < 0) v = !(cfg_get("MKW_SPARSE_IMAGES") && !atoi(cfg_get("MKW_SPARSE_IMAGES")));   // default on
    return v;
}

// Sparse memory in device-local memory, or when that is full or too small in any type req allows. An APU's own is a
// 256 MiB carve-out on Windows, and the terrain's tiles found no memory and drew black (issue #63, Ryzen 7 5700G: 57
// failures); the rest is the same RAM.
static VkResult alloc_local_or_any(VkCtx *c, const VkMemoryRequirements *req, VmaAllocation *a, VmaAllocationInfo *info) {
    if (!req->memoryTypeBits) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    VmaAllocationCreateInfo aci = {.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT};
    VkResult r = vmaAllocateMemory(c->vma, req, &aci, a, info);
    if (r == VK_SUCCESS) return r;
    aci = (VmaAllocationCreateInfo){.preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT};
    if ((r = vmaAllocateMemory(c->vma, req, &aci, a, info)) == VK_SUCCESS && !n_host_blocks++)
        rt_log("vk: sparse: memory outside the GPU's own (device-local full or too small)");
    return r;
}

// After a fill of slot memory, before anything reads or binds it (the queue order puts the upload batch first).
static const VkMemoryBarrier2 fill_mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                         .srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                                         .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                         .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                         .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
static const VkDependencyInfo fill_dep = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1,
                                          .pMemoryBarriers = &fill_mb};

static SlotBlock *new_block(VkCtx *c, uint32_t type_bits, VkDeviceSize slot_size) {
    SlotBlock *b = calloc(1, sizeof *b);
    if (!b) rt_fatal("vk: out of memory (sparse block)");
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = slot_size * BLOCK_SLOTS,
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};   // fills and capture readbacks
    if (vkCreateBuffer(c->device, &bi, NULL, &b->buf) != VK_SUCCESS) { free(b); return NULL; }
    VkMemoryRequirements rb;
    vkGetBufferMemoryRequirements(c->device, b->buf, &rb);
    VkMemoryRequirements req = {.size = slot_size * BLOCK_SLOTS, .alignment = slot_size > rb.alignment ? slot_size : rb.alignment,
                                .memoryTypeBits = rb.memoryTypeBits & type_bits};
    // #215 (GTX 750 Ti, 0.1.10): buffer types 0xe81, sparse image types 0x104, none in common, so every block failed
    // and the terrain had no tile at all. Then the block takes the image's types alone and has no buffer: its slots
    // start as the driver hands the memory out (not zero-filled here) and a reused slot keeps its last texels until
    // the game writes it. MKW_SPARSE_NO_BUFFER=1 takes that path on any GPU, to test it where the types meet.
    static int no_buf_env = -1;
    if (no_buf_env < 0) no_buf_env = cfg_get("MKW_SPARSE_NO_BUFFER") && atoi(cfg_get("MKW_SPARSE_NO_BUFFER"));
    if (!req.memoryTypeBits || no_buf_env) {
        static int logged;
        if (!logged++)
            rt_log("vk: sparse: tile blocks without a buffer (buffer types %#x, image types %#x%s): slots not zero-filled",
                   rb.memoryTypeBits, type_bits, no_buf_env ? ", MKW_SPARSE_NO_BUFFER" : "");
        vkDestroyBuffer(c->device, b->buf, NULL);
        b->buf = VK_NULL_HANDLE;
        req.memoryTypeBits = type_bits;
        req.alignment = slot_size;
    }
    VmaAllocationInfo info;
    VkResult r = alloc_local_or_any(c, &req, &b->alloc, &info);
    if (r != VK_SUCCESS) {
        // #215 (GTX 750 Ti): the first slot already failed with no slot live and VRAM to spare; the hypothesis is
        // that the buffer's memory types and the sparse image's do not meet on that driver (an empty mask fails here)
        static uint32_t seen[8][2];
        static int nseen;
        int known = 0;
        for (int i = 0; i < nseen; i++) known |= seen[i][0] == rb.memoryTypeBits && seen[i][1] == type_bits;
        if (!known && nseen < 8) {
            seen[nseen][0] = rb.memoryTypeBits, seen[nseen++][1] = type_bits;
            rt_log("vk: sparse: tile block of %llu KiB failed (%d): buffer types %#x, image types %#x, both %#x",
                   (unsigned long long)(req.size >> 10), r, rb.memoryTypeBits, type_bits, req.memoryTypeBits);
        }
    }
    if (r != VK_SUCCESS || (b->buf && vmaBindBufferMemory(c->vma, b->alloc, b->buf) != VK_SUCCESS)) {
        if (b->alloc) vmaFreeMemory(c->vma, b->alloc);
        if (b->buf) vkDestroyBuffer(c->device, b->buf, NULL);
        free(b);
        return NULL;
    }
    b->mem = info.deviceMemory, b->base = info.offset, b->type_bits = type_bits, b->slot_size = slot_size;
    n_blocks++;
    b->next = blocks, blocks = b;
    if (b->buf) {   // pool memory starts zeroed: the whole block once, instead of a fill and a barrier per slot (12 288
        VkCommandBuffer cb = vk_upload_begin();   // of them in the logo's 1 s frame, merge-on2 14/11/2026)
        vkCmdFillBuffer(cb, b->buf, 0, VK_WHOLE_SIZE, 0);
        vkCmdPipelineBarrier2(cb, &fill_dep);
        vk_upload_end();
    }
    return b;
}

// The slot of a physical page's block for an image, made (zeroed) on first use. NULL when it does not fit.
// A lane: one row of Vulkan blocks of a level (layer, level, row of NVN tiles ty, row sy inside the tile) being bound
// left to right (bind_page runs x first). Its slots come in order from one span of a block, so the row binds as one
// (make_bind's runs): with slots taken first-free, a 2x2 tile's sub-blocks were consecutive and the next tile's came
// 4 later, runs of 2 (merge-on1: 11 471 blocks as 4 691 binds, 427 ms). The last few lanes, round robin.
static int merge_on(void);
typedef struct { const SparseImg *s; uint32_t layer, l, ty, sy; SlotBlock *b; uint32_t next, end; } Lane;
#define LANES 8
static Lane lanes[LANES];
static unsigned lane_at;

static Lane *lane_get(const SparseImg *s, uint32_t layer, uint32_t l, uint32_t ty, uint32_t sy) {
    for (int i = 0; i < LANES; i++)
        if (lanes[i].s == s && lanes[i].layer == layer && lanes[i].l == l && lanes[i].ty == ty && lanes[i].sy == sy)
            return &lanes[i];
    Lane *L = &lanes[lane_at++ % LANES];
    *L = (Lane){s, layer, l, ty, sy, NULL, 0, 0};
    return L;
}

// The slot of a physical page's block for an image, made (zeroed) on first use. NULL when it does not fit. L: the
// lane the block is in and want the blocks left in its row, for a span of consecutive slots (NULL: first free).
static Slot *slot_get(VkCtx *c, const SparseImg *s, uint64_t pa, uint32_t sub, Lane *L, uint32_t want) {
    SlotKey k = {pa, sub, (int32_t)s->fmt};
    Slot *sl = nvn_hm_get(&slots, &k);
    if (sl) return sl;
    SlotBlock *b = NULL;
    uint32_t i = 0;
    int lane_ok = L && L->b && L->next < L->end && L->b->busy[L->next] != 1;
    if (lane_ok) {
        b = L->b, i = L->next;
    } else if (L && merge_on()) {   // a block with room above its top, for a span of this row
        for (b = blocks; b && (b->top == BLOCK_SLOTS || b->type_bits != s->type_bits); b = b->next) {}
    }
    if (!b) {
        b = blocks;
        while (b && (b->used == BLOCK_SLOTS || b->type_bits != s->type_bits)) b = b->next;
    }
    // Small images are outside the cap: it is there for the 8192x8192x68 arrays, which fill it before the 512x512 ones
    // the game makes later (foliage maps, r8sn) get a slot, and those read as zero drew the bushes black (01/11/2026).
    // The live slots count too: a block opened past the cap for a small image is not for the arrays to fill (m3).
    int big = s->size > SMALL_SPARSE;
    if (big && ((uint64_t)(n_slots + 1) * s->slot_size > budget() ||
                (!b && (uint64_t)(n_blocks + 1) * BLOCK_SLOTS * s->slot_size > budget()))) {
        if (++n_over <= 4 || !(n_over & (n_over - 1)))
            rt_log("vk: sparse: tile slots over MKW_SPARSE_MIB (%lld MiB in blocks; %lld tiles left non-resident)",
                   (long long)(budget() >> 20), (long long)n_over);
        return NULL;
    }
    if (!b && !(b = new_block(c, s->type_bits, s->slot_size))) {
        if (++n_failed <= 8 || !(n_failed & (n_failed - 1)))
            rt_log("vk: sparse: no memory for a tile slot (%lld failed, %lld slots live)", (long long)n_failed,
                   (long long)n_slots);
        return NULL;
    }
    if (!lane_ok) {
        if (L && merge_on() && b->top < BLOCK_SLOTS) {
            uint32_t span = want < BLOCK_SLOTS - b->top ? want : BLOCK_SLOTS - b->top;
            i = b->top, b->top += span;
            L->b = b, L->next = i, L->end = i + span;
        } else {
            while (b->busy[i] == 1) i++;
        }
    }
    if (L && L->b == b && L->next == i) L->next++;
    int reused = b->busy[i] == 2;   // a freed slot keeps its last texels: zeroed again here
    b->busy[i] = 1, b->used++;
    if (i >= b->top) b->top = i + 1;
    if (!(sl = calloc(1, sizeof *sl))) rt_fatal("vk: out of memory (sparse slot)");
    sl->key = k, sl->blk = b, sl->idx = i;
    nvn_hm_put(&slots, &k, sl);
    n_slots++;
    if (!b->buf) return sl;   // no buffer to fill through (new_block): neither zeros nor a capture's texels
    // A capture being replayed (runtime/gfx/cap) holds this slot's texels: they replace the zeros, each time the slot is
    // made (a slot no binding names is freed once the GPU is past it, which depends on timing, and may come back).
    uint8_t *saved = cap_pending.n ? nvn_hm_get(&cap_pending, &k) : NULL;
    if (!reused && !saved) return sl;   // zero from the block's fill (new_block)
    VkCommandBuffer cb = vk_upload_begin();
    if (saved) {
        VkBuffer sb;
        uint8_t *map = vk_staging(s->slot_size, &sb, NULL);
        memcpy(map, saved, s->slot_size);
        VkBufferCopy cp = {0, (VkDeviceSize)i * s->slot_size, s->slot_size};
        vkCmdCopyBuffer(cb, sb, b->buf, 1, &cp);
        sl->written = 1;
    } else {
        vkCmdFillBuffer(cb, b->buf, (VkDeviceSize)i * s->slot_size, s->slot_size, 0);
    }
    vkCmdPipelineBarrier2(cb, &fill_dep);
    vk_upload_end();
    return sl;
}

static void slot_unref(Slot *sl) {
    if (--sl->refs) return;
    if (ndying == cdying) grow((void **)&dying, &cdying, sizeof *dying);
    dying[ndying++] = sl;
    sl->reaps++;
}

// Completion thread, once the submission after the unbind is done: frees the slot unless a binding named it again.
static void slot_reap(void *arg) {
    Slot *sl = arg;
    plat_mutex_lock(&sp_lock);
    if (!--sl->reaps && !sl->refs) {
        nvn_hm_del(&slots, &sl->key);
        sl->blk->busy[sl->idx] = 2, sl->blk->used--;
        n_slots--;
        n_written -= sl->written;
        free(sl);
    }
    plat_mutex_unlock(&sp_lock);
}

static void mark_dirty(SparseImg *s, size_t i) {
    if (s->dirty[i]) return;
    s->dirty[i] = 1;
    if (!s->ndl) ndirty++;
    if (s->ndl == s->cdl) grow((void **)&s->dl, &s->cdl, sizeof *s->dl);
    s->dl[s->ndl++] = (uint32_t)i;
}

// Block i of s: its layer, level and position in blocks.
static void blk_pos(const SparseImg *s, uint32_t i, uint32_t *layer, uint32_t *l, uint32_t *bx, uint32_t *by) {
    uint32_t r = i % s->nblk, lv = 0;
    *layer = i / s->nblk;
    while (lv + 1 < s->nlev && r >= s->blk0[lv + 1]) lv++;
    r -= s->blk0[lv];
    *l = lv, *bx = r % s->bx[lv], *by = r / s->bx[lv];
}

// The bind of blocks i..i+run-1 of s (one row; run 1 without MKW_SPARSE_MERGE) to what bound[] names now. The blocks
// of one bind are laid out in its memory x first (Vulkan 1.3 §33.4.4), so a run of slots contiguous in one block binds
// as one. RADV makes one VA update (a GEM_VA ioctl, ~50 us, plus the kernel's wait for the VM) per row of blocks of
// each bind: bind 2 of the boot, 11 471 blocks of the 8192x8192x68 arrays one by one, took 576-995 ms in
// vkQueueBindSparse on the replay thread, the 1.5 s frame of the logo (carga-tl1, carga-io2, 14/11/2026).
static VkSparseImageMemoryBind make_bind(const SparseImg *s, uint32_t i, uint32_t run) {
    uint32_t layer, l, bx, by;
    blk_pos(s, i, &layer, &l, &bx, &by);
    uint32_t x = bx * s->gw, y = by * s->gh, w = run * s->gw;
    uint32_t lw = s->w >> l ? s->w >> l : 1, lh = s->h >> l ? s->h >> l : 1;
    const Slot *sl = s->bound[i];
    return (VkSparseImageMemoryBind){
        .subresource = {s->aspect, l, layer},
        .offset = {(int32_t)x, (int32_t)y, 0},
        .extent = {lw - x < w ? lw - x : w, lh - y < s->gh ? lh - y : s->gh, 1},
        .memory = sl ? sl->blk->mem : VK_NULL_HANDLE,
        .memoryOffset = sl ? sl->blk->base + (VkDeviceSize)sl->idx * s->slot_size : 0};
}

// MKW_SPARSE_MERGE (default 1; 0: one bind per block, as before 14/11/2026; "ab": in the B blocks only).
static int merge_on(void) {
    static int v = -1;
    if (v < 0) v = rt_knob("SPARSE_MERGE", "1");
    return RT_KNOB_ON(v);
}

static int by_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

// How many blocks from dl[k] (sorted) on make one row run bound to contiguous slots, or all unbound: one bind.
static int run_len(const SparseImg *s, int k) {
    uint32_t i = s->dl[k], layer, l, bx, by;
    blk_pos(s, i, &layer, &l, &bx, &by);
    const Slot *sl = s->bound[i];
    int run = 1;
    while (k + run < s->ndl && s->dl[k + run] == i + (uint32_t)run && bx + (uint32_t)run < s->bx[l]) {
        const Slot *nx = s->bound[i + (uint32_t)run];
        if (sl ? !(nx && nx->blk == sl->blk && nx->idx == sl->idx + (uint32_t)run) : nx != NULL) break;
        run++;
    }
    return run;
}

// Binds the Vulkan blocks inside page p of s to what the page maps now (sp_lock held).
static void bind_page(VkCtx *c, SparseImg *s, uint64_t p) {
    uint32_t layer = (uint32_t)(p / s->stride), rel = (uint32_t)(p % s->stride), l = 0;
    if (layer >= s->layers) return;   // the mip tails: never mapped by MKW
    while (l + 1 < s->nnvn && rel >= s->tile0[l + 1]) l++;
    if (l >= s->nlev) return;         // inside the Vulkan mip tail
    uint32_t t = rel - s->tile0[l], tx = t % s->tiles_x[l], ty = t / s->tiles_x[l];
    uint32_t lw = s->w >> l ? s->w >> l : 1, lh = s->h >> l ? s->h >> l : 1;
    uint64_t pa = 0;
    int mapped = vk_mem_virt_phys(s->addr + p * PAGE, &pa);

    for (uint32_t sy = 0; sy < s->nth / s->gh; sy++) {
        Lane *L = mapped ? lane_get(s, layer, l, ty, sy) : NULL;
        for (uint32_t sx = 0; sx < s->ntw / s->gw; sx++) {
            uint32_t x = tx * s->ntw + sx * s->gw, y = ty * s->nth + sy * s->gh;
            if (x >= lw || y >= lh) continue;
            size_t i = (size_t)layer * s->nblk + s->blk0[l] + (y / s->gh) * s->bx[l] + x / s->gw;
            Slot *want = mapped ? slot_get(c, s, pa, sy * (s->ntw / s->gw) + sx, L, s->bx[l] - x / s->gw) : NULL;
            if (want == s->bound[i]) continue;
            if (want) want->refs++;
            if (s->bound[i]) slot_unref(s->bound[i]);
            s->bound[i] = want;
            mark_dirty(s, i);
        }
    }
}

static void bind_range(VkCtx *c, SparseImg *s, uint64_t lo, uint64_t hi) {
    if (lo < s->addr) lo = s->addr;
    if (hi > s->addr + s->size) hi = s->addr + s->size;
    for (uint64_t a = lo & ~(uint64_t)(PAGE - 1); a < hi; a += PAGE) bind_page(c, s, (a - s->addr) / PAGE);
}

static uint32_t tail_count(const SparseImg *s) {   // mip tails of s: one a layer, or one for all
    if (s->tail_end == s->nmips || !s->tail_req.imageMipTailSize) return 0;
    return s->tail_req.formatProperties.flags & VK_SPARSE_IMAGE_FORMAT_SINGLE_MIPTAIL_BIT ? 1 : s->layers;
}

static VkDeviceSize round_up(VkDeviceSize v, VkDeviceSize a) { return (v + a - 1) / a * a; }

// Memory for the levels of s outside NVN's tiles (see tail_alloc in SparseImg); levels: the image's mip count.
static void tail_alloc(VkCtx *c, SparseImg *s, const VkSparseImageMemoryRequirements *sr, uint32_t levels, uint32_t types) {
    s->nmips = levels, s->tail_end = sr && sr->imageMipTailFirstLod < levels ? sr->imageMipTailFirstLod : levels;
    if (sr) s->tail_req = *sr;
    for (uint32_t l = s->nlev; l < s->tail_end; l++) {
        uint32_t lw = s->w >> l ? s->w >> l : 1, lh = s->h >> l ? s->h >> l : 1;
        s->tail_blocks += ((lw + s->gw - 1) / s->gw) * ((lh + s->gh - 1) / s->gh);
    }
    VkDeviceSize bytes = (VkDeviceSize)s->layers * s->tail_blocks * s->slot_size +
                         tail_count(s) * round_up(s->tail_req.imageMipTailSize, s->slot_size);
    if (!bytes) return;
    VkMemoryRequirements req = {.size = bytes, .alignment = s->slot_size, .memoryTypeBits = types};
    VmaAllocationInfo info;
    if (alloc_local_or_any(c, &req, &s->tail_alloc, &info) != VK_SUCCESS) {
        s->tail_alloc = NULL;
        rt_log("vk: sparse: no memory for the %llu KiB of levels outside the tiles (%ux%u layers %u)",
               (unsigned long long)(bytes >> 10), s->w, s->h, s->layers);
        return;
    }
    s->tail_mem = info.deviceMemory, s->tail_base = info.offset, s->tail_pending = 1;
    rt_log("vk: sparse: %ux%u layers %u: levels %u-%u in blocks (%u a layer), mip tail from %u: offset %#llx size %#llx "
           "stride %#llx flags %#x", s->w, s->h, s->layers, s->nlev, s->tail_end - 1, s->tail_blocks, s->tail_end,
           (unsigned long long)s->tail_req.imageMipTailOffset, (unsigned long long)s->tail_req.imageMipTailSize,
           (unsigned long long)s->tail_req.imageMipTailStride, s->tail_req.formatProperties.flags);
}

// The binds of s's levels outside the tiles: blocks into ib/bb, mip tails into ob/mb (sp_lock held).
static void tail_binds(const SparseImg *s, VkSparseImageMemoryBindInfo *ib, VkSparseImageMemoryBind *bb,
                       VkSparseImageOpaqueMemoryBindInfo *ob, VkSparseMemoryBind *mb) {
    uint32_t n = 0;
    VkDeviceSize off = s->tail_base;
    for (uint32_t layer = 0; layer < s->layers; layer++)
        for (uint32_t l = s->nlev; l < s->tail_end; l++) {
            uint32_t lw = s->w >> l ? s->w >> l : 1, lh = s->h >> l ? s->h >> l : 1;
            for (uint32_t y = 0; y < lh; y += s->gh)
                for (uint32_t x = 0; x < lw; x += s->gw, off += s->slot_size)
                    bb[n++] = (VkSparseImageMemoryBind){.subresource = {s->aspect, l, layer},
                                                        .offset = {(int32_t)x, (int32_t)y, 0},
                                                        .extent = {lw - x < s->gw ? lw - x : s->gw, lh - y < s->gh ? lh - y : s->gh, 1},
                                                        .memory = s->tail_mem, .memoryOffset = off};
        }
    *ib = (VkSparseImageMemoryBindInfo){.image = s->img, .bindCount = n, .pBinds = bb};
    uint32_t nt = tail_count(s);
    for (uint32_t t = 0; t < nt; t++, off += round_up(s->tail_req.imageMipTailSize, s->slot_size))
        mb[t] = (VkSparseMemoryBind){.resourceOffset = s->tail_req.imageMipTailOffset + t * s->tail_req.imageMipTailStride,
                                     .size = s->tail_req.imageMipTailSize, .memory = s->tail_mem, .memoryOffset = off};
    *ob = (VkSparseImageOpaqueMemoryBindInfo){.image = s->img, .bindCount = nt, .pBinds = mb};
}

static void tail_free(void *arg) { vmaFreeMemory(vk_ctx()->vma, arg); }

static int order_on(void);

// Binds s's levels outside the tiles now, on the timeline every graphics-queue submission waits on (vk_sparse_order):
// the game copies into them right after creating the array, and a copy recorded in the upload batch (ASTC decoded
// into BC7, vk_texture_astc_copy) went out before a bind left for the next replayed submission and was lost (m17).
static void tail_bind_now(VkCtx *c, SparseImg *s) {
    if (!s->tail_pending || !order_on()) return;   // MKW_SPARSE_ORDER=0: with the other binds (vk_sparse_order)
    VkSparseImageMemoryBindInfo ib;
    VkSparseImageOpaqueMemoryBindInfo ob;
    VkSparseImageMemoryBind *bb = malloc(sizeof *bb * ((size_t)s->layers * s->tail_blocks + 1));
    VkSparseMemoryBind *mb = malloc(sizeof *mb * (tail_count(s) + 1));
    if (!bb || !mb) rt_fatal("vk: out of memory (sparse tail binds)");
    tail_binds(s, &ib, bb, &ob, mb);
    vk_queue_lock();
    if (!order_tl) {
        VkSemaphoreTypeCreateInfo ti = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
        VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &ti};
        if (vkCreateSemaphore(c->device, &si, NULL, &order_tl) != VK_SUCCESS) rt_fatal("vk: sparse order semaphore");
    }
    uint64_t wait = order_val, sig = ++order_val;
    VkTimelineSemaphoreSubmitInfo tl = {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                                        .waitSemaphoreValueCount = wait ? 1u : 0u, .pWaitSemaphoreValues = &wait,
                                        .signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &sig};
    VkBindSparseInfo bi = {.sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO, .pNext = &tl,
                           .waitSemaphoreCount = wait ? 1u : 0u, .pWaitSemaphores = &order_tl,
                           .imageOpaqueBindCount = ob.bindCount ? 1u : 0u, .pImageOpaqueBinds = &ob,
                           .imageBindCount = ib.bindCount ? 1u : 0u, .pImageBinds = &ib,
                           .signalSemaphoreCount = 1, .pSignalSemaphores = &order_tl};
    VkResult r = vkQueueBindSparse(c->gfx_queue, 1, &bi, VK_NULL_HANDLE);
    bind_val = sig;
    s->bind_fence = vk_sync_issued() + 2;   // the submission being recorded may have taken its wait already
    vk_queue_unlock();
    if (r != VK_SUCCESS) rt_fatal("vk: vkQueueBindSparse of the levels outside the tiles failed (%d)", (int)r);
    free(bb);
    free(mb);
    s->tail_pending = 0;
}

// imageGranularity is "in texels or compressed texel blocks": NVIDIA gives BC7 256x256 (texels), Intel's Windows driver
// 64x64 (blocks), both the standard 64 KiB block. Binds go in texels, so 64x64 taken as texels bound each block 16 times
// and spent 16 times the slots: 1 048 576 tiles left non-resident on an Arc B580 against 32 768 on an RTX 3050, roads
// and trees black or missing (#476, #475, #355). A BC format whose granularity as texels is not 64 KiB but is as
// blocks is converted.
void vk_sparse_granularity_texels(VkFormat f, uint32_t *gw, uint32_t *gh) {
    if (f < VK_FORMAT_BC1_RGB_UNORM_BLOCK || f > VK_FORMAT_BC7_SRGB_BLOCK) return;
    int half = f <= VK_FORMAT_BC1_RGBA_SRGB_BLOCK || f == VK_FORMAT_BC4_UNORM_BLOCK || f == VK_FORMAT_BC4_SNORM_BLOCK;
    uint64_t bpb = half ? 8 : 16;
    if ((uint64_t)(*gw / 4) * (*gh / 4) * bpb == 65536 || (uint64_t)*gw * *gh * bpb != 65536) return;
    static int logged;
    if (!logged++)
        rt_log("vk: sparse: the driver gives compressed formats' block size in 4x4 blocks (%ux%u for VkFormat %d): "
               "taken as %ux%u texels", *gw, *gh, (int)f, *gw * 4, *gh * 4);
    *gw *= 4, *gh *= 4;
}

int vk_sparse_create(const VkImageCreateInfo *ci0, const nvn_texdesc *d, uint64_t addr, uint64_t size, VkImage *out,
                     VkSparseTail *tail) {
    VkCtx *c = vk_ctx();
    static volatile int32_t nwarn;
    *out = VK_NULL_HANDLE;
    *tail = (VkSparseTail){0};
    if (!on() || !(d->flags & NVN_TEXTURE_FLAGS_SPARSE) || !nvn_gpu_only(addr) || ci0->imageType != VK_IMAGE_TYPE_2D ||
        ci0->samples != VK_SAMPLE_COUNT_1_BIT || !(ci0->usage & VK_IMAGE_USAGE_SAMPLED_BIT))
        return 0;
    VkImageCreateInfo ci = *ci0;
    // Only cube targets take cube views (vk_exec_view.c). Unneeded cube compatibility on the terrain's 2D arrays
    // makes non-base mips sample zeros on RX 570 / Windows 26.5.2 even though transfer readbacks are correct.
    if (d->target != NVN_TEXTURE_TARGET_CUBEMAP && d->target != NVN_TEXTURE_TARGET_CUBEMAP_ARRAY)
        ci.flags &= ~(VkImageCreateFlags)VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    ci.flags |= VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT | VK_IMAGE_CREATE_SPARSE_ALIASED_BIT;
    ci.usage &= ~(VkImageUsageFlags)VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;
    uint32_t n = 0;
    VkSparseImageFormatProperties fp[4];
    vkGetPhysicalDeviceSparseImageFormatProperties(c->phys, ci.format, ci.imageType, ci.samples, ci.usage, ci.tiling, &n, NULL);
    if (n > 4) n = 4;
    vkGetPhysicalDeviceSparseImageFormatProperties(c->phys, ci.format, ci.imageType, ci.samples, ci.usage, ci.tiling, &n, fp);
    if (!n || (fp[0].flags & VK_SPARSE_IMAGE_FORMAT_NONSTANDARD_BLOCK_SIZE_BIT)) {
        if (plat_inc32(&nwarn) <= 8) rt_log("vk: sparse: VkFormat %d usage %#x has no standard sparse blocks", (int)ci.format, ci.usage);
        return 0;
    }
    int32_t lay[4];   // levels with whole tiles, tile width, height, depth
    nvn_sparse_tile_layout(d, lay);
    SparseImg *s = calloc(1, sizeof *s);
    if (!s) rt_fatal("vk: out of memory (sparse image)");
    s->addr = addr, s->size = size, s->fmt = ci.format, s->aspect = fp[0].aspectMask;
    s->w = ci.extent.width, s->h = ci.extent.height, s->layers = ci.arrayLayers;
    s->ntw = (uint32_t)lay[1], s->nth = (uint32_t)lay[2];
    s->gw = fp[0].imageGranularity.width, s->gh = fp[0].imageGranularity.height;
    vk_sparse_granularity_texels(ci.format, &s->gw, &s->gh);
    if (!s->gw || !s->gh || s->ntw % s->gw || s->nth % s->gh || lay[0] > 16) {
        if (plat_inc32(&nwarn) <= 8)
            rt_log("vk: sparse: NVN tile %ux%u is not whole Vulkan blocks of %ux%u (VkFormat %d)", s->ntw, s->nth, s->gw,
                   s->gh, (int)ci.format);
        free(s);
        return 0;
    }
    VkResult cr = vkCreateImage(c->device, &ci, NULL, &s->img);
    if (cr != VK_SUCCESS) {
        // The virtual range of a sparse image is reserved at creation: on RADV GFX8 (Polaris, #314) the 8192x8192x68
        // arrays failed, likely past the device's sparse address space. Its size, ours so far and the device's say.
        if (plat_inc32(&nwarn) <= 8) {
            VkDeviceImageMemoryRequirements q = {.sType = VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS, .pCreateInfo = &ci};
            VkMemoryRequirements2 r2 = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
            vkGetDeviceImageMemoryRequirements(c->device, &q, &r2);
            plat_mutex_lock(&sp_lock);
            uint64_t live = sparse_va;
            plat_mutex_unlock(&sp_lock);
            rt_log("vk: sparse: vkCreateImage failed (%d; VkFormat %d %ux%u layers %u levels %u, %llu MiB of address space; "
                   "%llu MiB in %lld sparse images live, device sparse address space %llu MiB)", (int)cr, (int)ci.format,
                   s->w, s->h, ci.arrayLayers, ci.mipLevels, (unsigned long long)(r2.memoryRequirements.size >> 20),
                   (unsigned long long)(live >> 20), (long long)n_images,
                   (unsigned long long)(c->props.limits.sparseAddressSpaceSize >> 20));
        }
        free(s);
        return 0;
    }
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(c->device, s->img, &mr);
    s->va_size = mr.size;
    uint32_t nr = 1;
    VkSparseImageMemoryRequirements sr;
    vkGetImageSparseMemoryRequirements(c->device, s->img, &nr, &sr);
    s->slot_size = mr.alignment, s->type_bits = mr.memoryTypeBits;
    s->nlev = (uint32_t)lay[0] < ci.mipLevels ? (uint32_t)lay[0] : ci.mipLevels;
    if (nr && sr.imageMipTailFirstLod < s->nlev) s->nlev = sr.imageMipTailFirstLod;   // not seen on RADV: log below
    s->nnvn = (uint32_t)lay[0];
    for (uint32_t l = 0, page = 0, blk = 0; l < s->nnvn; l++) {
        uint32_t lw = s->w >> l ? s->w >> l : 1, lh = s->h >> l ? s->h >> l : 1;
        s->tiles_x[l] = (lw + s->ntw - 1) / s->ntw, s->tiles_y[l] = (lh + s->nth - 1) / s->nth, s->tile0[l] = page;
        page += s->tiles_x[l] * s->tiles_y[l];
        s->stride = page;
        if (l < s->nlev) {
            s->bx[l] = (lw + s->gw - 1) / s->gw, s->blk0[l] = blk;
            blk += s->bx[l] * ((lh + s->gh - 1) / s->gh);
            s->nblk = blk;
        }
    }
    if (!(s->bound = calloc((size_t)s->layers * s->nblk, sizeof *s->bound)) ||
        !(s->dirty = calloc((size_t)s->layers * s->nblk, 1)))
        rt_fatal("vk: out of memory (sparse table)");
    tail_alloc(c, s, nr ? &sr : NULL, ci.mipLevels, mr.memoryTypeBits);
    tail_bind_now(c, s);
    plat_mutex_lock(&sp_lock);
    s->next = images, images = s;
    if (s->tail_pending) ntail_pending++;
    n_images++;
    sparse_va += s->va_size;
    // Coarse levels first, so a full budget leaves fine detail out rather than whole layers.
    for (int l = (int)s->nlev - 1; l >= 0; l--)
        for (uint32_t y = 0; y < s->layers; y++) {
            uint64_t lo = s->addr + ((uint64_t)y * s->stride + s->tile0[l]) * PAGE;
            bind_range(c, s, lo, lo + (uint64_t)s->tiles_x[l] * s->tiles_y[l] * PAGE);
        }
    int64_t ni = n_images, ns = n_slots;
    plat_mutex_unlock(&sp_lock);
    if (ni <= 16 || !(ni & (ni - 1)))
        rt_log("vk: sparse image %lld: %#llx %ux%u layers %u VkFormat %d, NVN tile %ux%u (%u levels, %u pages a layer), "
               "Vulkan block %ux%u of %llu bytes, mip tail from level %u, %llu MiB of address space; %lld slots live",
               (long long)ni,
               (unsigned long long)addr, s->w, s->h, s->layers, (int)ci.format, s->ntw, s->nth, (unsigned)lay[0],
               s->stride, s->gw, s->gh, (unsigned long long)s->slot_size, nr ? sr.imageMipTailFirstLod : 0,
               (unsigned long long)(s->va_size >> 20), (long long)ns);
    *out = s->img;
    if (s->tail_alloc) {
        uint32_t w = s->w >> s->nlev, h = s->h >> s->nlev;
        w = w ? w : 1, h = h ? h : 1;
        // A whole sparse block per covered tile bounds the tight linear bytes of one layer, including compressed formats.
        *tail = (VkSparseTail){s->nlev, s->aspect,
                              (VkDeviceSize)((w + s->gw - 1) / s->gw) * ((h + s->gh - 1) / s->gh) * s->slot_size};
    }
    return 1;
}

uint64_t vk_sparse_destroy(VkImage img) {
    uint64_t bind_fence = 0;
    plat_mutex_lock(&sp_lock);
    SparseImg **pp = &images;
    while (*pp && (*pp)->img != img) pp = &(*pp)->next;
    SparseImg *s = *pp;
    if (s) {
        *pp = s->next;
        bind_fence = s->bind_fence;
        for (size_t i = 0; i < (size_t)s->layers * s->nblk; i++)
            if (s->bound[i]) slot_unref(s->bound[i]);
        if (s->ndl) ndirty--;   // binds not issued yet: the image goes away first
        if (s->tail_pending) ntail_pending--;
        if (s->tail_alloc) {
            if (ndying_tails == cdying_tails) grow((void **)&dying_tails, &cdying_tails, sizeof *dying_tails);
            dying_tails[ndying_tails++] = s->tail_alloc;
        }
        free(s->bound);
        free(s->dirty);
        free(s->dl);
        sparse_va -= s->va_size;
        free(s);
        n_images--;
    }
    plat_mutex_unlock(&sp_lock);
    return bind_fence;
}

int vk_sparse_is(VkImage img) {
    plat_mutex_lock(&sp_lock);
    const SparseImg *s = images;
    while (s && s->img != img) s = s->next;
    plat_mutex_unlock(&sp_lock);
    return s != NULL;
}

void vk_sparse_remap(uint64_t lo, uint64_t size) {
    plat_mutex_lock(&sp_lock);
    VkCtx *c = vk_ctx();
    for (SparseImg *s = images; s; s = s->next)
        if (lo < s->addr + s->size && s->addr < lo + size) bind_range(c, s, lo, lo + size);
    plat_mutex_unlock(&sp_lock);
}

// Eglantine (d4ee7fcd51) found 13-26% of MKW's sparse terrain blocks written and nine layer pools never written: this
// counts the slots a copy or a render pass writes, to measure how much slot memory only ever holds zeros. w == 0: the
// whole level.
void vk_sparse_written(VkImage img, uint32_t level, uint32_t layer0, uint32_t layers, int32_t x, int32_t y, uint32_t w,
                       uint32_t h) {
    plat_mutex_lock(&sp_lock);
    SparseImg *s = images;
    while (s && s->img != img) s = s->next;
    if (s && level >= s->nlev && level < 32 && !(s->tail_written >> level & 1)) {
        s->tail_written |= 1u << level, n_tail_writes++;
        rt_log("vk: sparse: first write to level %u of %ux%u layers %u (outside the tiles; %s)", level, s->w, s->h,
               s->layers, s->tail_alloc ? "has memory" : "no memory: lost");
    }
    if (s && level < s->nlev) {
        uint32_t lw = s->w >> level ? s->w >> level : 1, lh = s->h >> level ? s->h >> level : 1;
        if (!w) x = y = 0, w = lw, h = lh;
        uint32_t bx0 = (uint32_t)x / s->gw, by0 = (uint32_t)y / s->gh;
        uint32_t bx1 = ((uint32_t)x + w + s->gw - 1) / s->gw, by1 = ((uint32_t)y + h + s->gh - 1) / s->gh;
        uint32_t rows = (lh + s->gh - 1) / s->gh;
        if (bx1 > s->bx[level]) bx1 = s->bx[level];
        if (by1 > rows) by1 = rows;
        for (uint32_t l = layer0; l < layer0 + layers && l < s->layers; l++)
            for (uint32_t by = by0; by < by1; by++)
                for (uint32_t bx = bx0; bx < bx1; bx++) {
                    Slot *sl = s->bound[(size_t)l * s->nblk + s->blk0[level] + by * s->bx[level] + bx];
                    if (sl && !sl->written) sl->written = 1, n_written++;
                }
    }
    plat_mutex_unlock(&sp_lock);
}

// Queue lock held (it guards order_tl and its values), before a submission to the graphics queue: gives the order of
// that submission (wait for the last bind, signal its own value). binds: first issue the queued binds behind every
// earlier submission (vk_sync.c; not vk_upload_flush, which holds cmd_lock, taken under sp_lock by slot_get). fence:
// the timeline value whose completion frees the slots unbound so far (a part or the end of a replayed submission).
// MKW_SPARSE_ORDER=0 (A/B): the order before 31/10/2026, binds behind the previous frame's value only, and no wait or
// signal on the other submissions. binds 2: only issue the queued binds, before the upload batch that a replayed part
// flushes ahead of itself (vk_sync.c), so the batch's copies into sparse levels land in bound memory.
static int order_on(void) {
    static int v = -1;
    if (v < 0) v = !(cfg_get("MKW_SPARSE_ORDER") && !atoi(cfg_get("MKW_SPARSE_ORDER")));
    return v;
}

VkSparseOrder vk_sparse_order(uint64_t fence, int binds) {
    VkCtx *c = vk_ctx();
    int on_ = order_on();
    if (!on_ && binds != 1) return (VkSparseOrder){0};
    if (on_ && !order_tl) {
        VkSemaphoreTypeCreateInfo ti = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
        VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &ti};
        if (vkCreateSemaphore(c->device, &si, NULL, &order_tl) != VK_SUCCESS) rt_fatal("vk: sparse order semaphore");
    }
    if (!binds) return (VkSparseOrder){order_tl, order_tl, bind_val, ++order_val};
    plat_mutex_lock(&sp_lock);
    if (fence) {
        for (int i = 0; i < ndying; i++) vk_completion_add(fence, slot_reap, dying[i]);
        ndying = 0;
        for (int i = 0; i < ndying_tails; i++) vk_completion_add(fence, tail_free, dying_tails[i]);
        ndying_tails = 0;
    }
    if (ndirty || ntail_pending) {
        int ni = 0, nb = 0, total = 0, no = 0, nm = 0, mtotal = 0;
        for (SparseImg *s = images; s; s = s->next) {
            total += s->ndl;
            if (s->tail_pending) total += (int)(s->layers * s->tail_blocks), mtotal += (int)tail_count(s);
        }
        VkSparseImageMemoryBindInfo *ib = malloc(sizeof *ib * (size_t)(ndirty + ntail_pending));
        VkSparseImageMemoryBind *bb = malloc(sizeof *bb * (size_t)(total ? total : 1));
        VkSparseImageOpaqueMemoryBindInfo *ob = malloc(sizeof *ob * (size_t)(ntail_pending ? ntail_pending : 1));
        VkSparseMemoryBind *mb = malloc(sizeof *mb * (size_t)(mtotal ? mtotal : 1));
        if (!ib || !bb || !ob || !mb) rt_fatal("vk: out of memory (sparse binds)");
        for (SparseImg *s = images; s; s = s->next) {
            if (!s->tail_pending) continue;
            tail_binds(s, &ib[ni], bb + nb, &ob[no], mb + nm);
            s->bind_fence = fence;
            nb += (int)ib[ni].bindCount, nm += (int)ob[no].bindCount;
            if (ib[ni].bindCount) ni++;
            if (ob[no].bindCount) no++;
            s->tail_pending = 0;
        }
        ntail_pending = 0;
        int blocks = 0, merge = merge_on(), run1 = 0, run_max = 0;
        for (SparseImg *s = images; s; s = s->next) {
            if (!s->ndl) continue;
            if (merge) qsort(s->dl, (size_t)s->ndl, sizeof *s->dl, by_u32);
            int nb0 = nb;
            s->bind_fence = fence;   // submission `fence` waits for this bind (bind_sem, or order_tl)
            for (int k = 0; k < s->ndl;) {
                int run = merge ? run_len(s, k) : 1;
                run1 += run == 1, run_max = run > run_max ? run : run_max;
                bb[nb++] = make_bind(s, s->dl[k], (uint32_t)run);
                for (int j = 0; j < run; j++) s->dirty[s->dl[k + j]] = 0;
                k += run;
            }
            ib[ni++] = (VkSparseImageMemoryBindInfo){.image = s->img, .bindCount = (uint32_t)(nb - nb0), .pBinds = bb + nb0};
            blocks += s->ndl;
            s->ndl = 0;
        }
        ndirty = 0;
        uint64_t wait = on_ ? order_val : fence - 1, sig = on_ ? ++order_val : 0;
        if (!on_ && !bind_sem) {
            VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            if (vkCreateSemaphore(c->device, &si, NULL, &bind_sem) != VK_SUCCESS) rt_fatal("vk: sparse bind semaphore");
        }
        VkTimelineSemaphoreSubmitInfo tl = {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                                            .waitSemaphoreValueCount = wait ? 1u : 0u, .pWaitSemaphoreValues = &wait,
                                            .signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &sig};
        VkBindSparseInfo bi = {.sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO, .pNext = &tl,
                               .waitSemaphoreCount = wait ? 1u : 0u, .pWaitSemaphores = on_ ? &order_tl : &c->timeline,
                               .imageOpaqueBindCount = (uint32_t)no, .pImageOpaqueBinds = ob,
                               .imageBindCount = (uint32_t)ni, .pImageBinds = ib, .signalSemaphoreCount = 1,
                               .pSignalSemaphores = on_ ? &order_tl : &bind_sem};
        if (on_) bind_val = sig;
        else binary_pending = 1;
        uint64_t t0 = plat_now_ns();
        VkResult r = vkQueueBindSparse(c->gfx_queue, 1, &bi, VK_NULL_HANDLE);
        uint64_t dt = plat_now_ns() - t0;
        if (r != VK_SUCCESS) {
            if (r == VK_ERROR_DEVICE_LOST) vk_exec_recent_log();
            rt_fatal("vk: vkQueueBindSparse of %d binds failed (%d)", nb, (int)r);
        }
        n_binds += blocks;
        int64_t calls = 0;
        static int64_t ncalls;
        static uint64_t bind_ns;   // time in vkQueueBindSparse: the driver may update the page tables in the call
        calls = ++ncalls, bind_ns += dt;
        if (calls <= 4 || !(calls & (calls - 1)))
            rt_log("vk: sparse bind %lld: %d blocks in %d images as %d binds (%d single, longest %d; %lld blocks so far, "
                   "%lld slots live, %lld MiB, %lld written), %.1f ms in the call (%.1f so far)", (long long)calls, blocks,
                   ni, nb, run1, run_max, (long long)n_binds, (long long)n_slots, (long long)(n_slots * 65536 >> 20),
                   (long long)n_written, dt / 1e6, bind_ns / 1e6);
        free(ib);
        free(bb);
        free(ob);
        free(mb);
    }
    if (binds == 2) {   // the binds only: no submission follows to take a value
        plat_mutex_unlock(&sp_lock);
        return (VkSparseOrder){0};
    }
    VkSparseOrder o = {order_tl, order_tl, bind_val, ++order_val};
    if (!on_) o = (VkSparseOrder){binary_pending ? bind_sem : VK_NULL_HANDLE, VK_NULL_HANDLE, 0, 0}, binary_pending = 0;
    plat_mutex_unlock(&sp_lock);
    return o;
}

// ---- runtime/gfx/cap: the written slots out (key and texels), and back in as the slots are made again
uint64_t vk_sparse_dump(void (*emit)(const void *key, uint32_t klen, const void *p, uint64_t n, void *ctx), void *ctx) {
    vk_sync_fence_wait(vk_sync_issued(), UINT64_MAX);
    vk_upload_flush(1);
    plat_mutex_lock(&sp_lock);
    uint64_t bytes = 0;
    for (uint32_t i = 0; i < slots.cap; i++) {
        Slot *sl = slots.vals ? slots.vals[i] : NULL;
        if (!sl || !sl->written || !sl->blk->buf) continue;
        VkDeviceSize n = sl->blk->slot_size;
        VkBuffer sb;
        VmaAllocation sa;
        uint8_t *map = vk_staging(n, &sb, &sa);
        VkCommandBuffer cb = vk_upload_begin();
        VkBufferCopy cp = {(VkDeviceSize)sl->idx * n, 0, n};
        vkCmdCopyBuffer(cb, sl->blk->buf, sb, 1, &cp);
        vk_upload_end();
        vk_upload_flush(1);
        vmaInvalidateAllocation(vk_ctx()->vma, sa, 0, VK_WHOLE_SIZE);
        emit(&sl->key, sizeof sl->key, map, n, ctx);
        vmaDestroyBuffer(vk_ctx()->vma, sb, sa);
        bytes += n;
    }
    plat_mutex_unlock(&sp_lock);
    return bytes;
}

void vk_sparse_load(const void *key, uint32_t klen, const void *p, uint64_t n) {
    if (klen != sizeof(SlotKey)) rt_fatal("vk: sparse slot key of %u bytes in a capture", klen);
    uint8_t *copy = malloc(n);
    if (!copy) rt_fatal("vk: out of memory (sparse slot)");
    memcpy(copy, p, n);
    plat_mutex_lock(&sp_lock);
    free(nvn_hm_put(&cap_pending, key, copy));
    plat_mutex_unlock(&sp_lock);
}

// The replay and completion threads have stopped, textures are gone and the device is idle. Slot blocks are kept for
// reuse during play, but their buffers/allocations and the bind-order semaphores must go before their owners.
void vk_sparse_shutdown(void) {
    VkCtx *c = vk_ctx();
    if (!c || !c->device) return;
    plat_mutex_lock(&sp_lock);
    for (uint32_t i = 0; i < slots.cap; i++) free(slots.vals ? slots.vals[i] : NULL);
    free(slots.keys), free(slots.vals);
    slots = (nvn_hmap){.klen = sizeof(SlotKey)};
    for (uint32_t i = 0; i < cap_pending.cap; i++) free(cap_pending.vals ? cap_pending.vals[i] : NULL);
    free(cap_pending.keys), free(cap_pending.vals);
    cap_pending = (nvn_hmap){.klen = sizeof(SlotKey)};
    while (blocks) {
        SlotBlock *b = blocks;
        blocks = b->next;
        if (b->buf) vkDestroyBuffer(c->device, b->buf, NULL);
        if (b->alloc) vmaFreeMemory(c->vma, b->alloc);
        free(b);
    }
    for (int i = 0; i < ndying_tails; i++) vmaFreeMemory(c->vma, dying_tails[i]);
    free(dying_tails), free(dying);
    dying_tails = NULL, dying = NULL;
    ndying_tails = cdying_tails = ndying = cdying = ndirty = ntail_pending = 0;
    if (order_tl) vkDestroySemaphore(c->device, order_tl, NULL);
    if (bind_sem) vkDestroySemaphore(c->device, bind_sem, NULL);
    order_tl = bind_sem = VK_NULL_HANDLE;
    order_val = bind_val = sparse_va = 0;
    binary_pending = 0;
    n_slots = n_failed = n_binds = n_images = n_blocks = n_over = n_written = n_tail_writes = n_host_blocks = 0;
    memset(lanes, 0, sizeof lanes);
    lane_at = 0;
    plat_mutex_unlock(&sp_lock);
}
