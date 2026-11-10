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

// ---------------------------------------------------------------- images
static VkFormatFeatureFlags features_of(VkFormat f) {
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(vk_ctx()->phys, f, &p);
    return p.optimalTilingFeatures;
}

static VkImageAspectFlags aspect_of(const TexVkFormat *f) {
    VkImageAspectFlags a = 0;
    if (f->depth) a |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if (f->stencil) a |= VK_IMAGE_ASPECT_STENCIL_BIT;
    return a ? a : VK_IMAGE_ASPECT_COLOR_BIT;
}

// Binds img into the pool memory behind its GPU address (physical pools, virtual pools through the page table) when
// the memory type, alignment and contiguous size allow; 0 otherwise.
static int format_compressed(VkFormat f) {   // vkCmdClearColorImage takes no block-compressed format
    return (f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK) ||
           (f >= VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK && f <= VK_FORMAT_ASTC_12x12_SFLOAT_BLOCK);
}

static int bind_pool_memory(VkCtx *c, Tex *e, VkImage img) {
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(c->device, img, &req);
    VkMemSpan sp;
    if (!vk_mem_resolve(e->pub.addr, req.size, &sp) || !sp.alloc || sp.alloc_offset % req.alignment) return 0;
    VmaAllocationInfo info;
    vmaGetAllocationInfo(c->vma, sp.alloc, &info);
    if (!(req.memoryTypeBits >> info.memoryType & 1)) return 0;
    if (vmaBindImageMemory2(c->vma, sp.alloc, sp.alloc_offset, img, NULL) != VK_SUCCESS) return 0;
    e->bound_to = sp.alloc;
    return 1;
}

// MKW_CTL "tex ADDR" (ctl.c): every live texture whose storage covers addr, with the size NVN gives its storage and the
// size of its Vulkan image, which is what a pool-bound image really occupies from its address on; addr 0: every
// pool-bound image whose Vulkan image is larger than its NVN storage (it spills into the memory after it).
void vk_texture_report(uint64_t addr) {
    plat_once(&tex_once, tex_init, NULL);
    plat_rmutex_lock(&tex_lock);
    VkCtx *c = vk_ctx();
    int n = 0, spill = 0;
    for (uint32_t i = 0; i < by_key.cap; i++) {
        const Tex *e = by_key.vals ? by_key.vals[i] : NULL;
        if (!e || !e->pub.image) continue;
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(c->device, e->pub.image, &req);
        uint64_t nsz = e->t.L.size, vsz = e->bound_to ? req.size : nsz, end = e->pub.addr + (vsz > nsz ? vsz : nsz);
        int over = e->bound_to && req.size > nsz;
        spill += over;
        if (addr ? !(addr >= e->pub.addr && addr < end) : !over) continue;
        if (++n > 64) continue;
        uint64_t pa = 0, pa_end = 0;   // a virtual pool's pages: the physical address of the first and last byte
        int virt = vk_mem_virt_phys(e->pub.addr, &pa) && vk_mem_virt_phys(e->pub.addr + nsz - 1, &pa_end);
        VkMemSpan sp;
        int contig = vk_mem_resolve(e->pub.addr, nsz, &sp);
        rt_log("ctl: tex %#llx fmt %#x %ux%ux%u levels %u: nvn %#llx bytes, vulkan %#llx (%s)%s%s%s%s phys %#llx..%#llx%s",
               (unsigned long long)e->pub.addr, e->key.d.format, e->key.d.width, e->key.d.height,
               e->key.d.depth, e->key.d.levels, (unsigned long long)nsz,
               (unsigned long long)req.size, e->bound_to ? "in pool memory" : "own memory", e->pub.gpu_owned ? ", gpu" : "",
               e->pub.mirrored ? ", mirrored" : "", e->released ? ", released" : "", e->sparse ? ", sparse" : "",
               virt ? (unsigned long long)pa : 0ull, virt ? (unsigned long long)pa_end : 0ull,
               contig ? ", one contiguous span" : ", not one span");
    }
    rt_log("ctl: tex %#llx: %d textures listed; %d pool-bound images larger than their storage", (unsigned long long)addr,
           n, spill);
    plat_rmutex_unlock(&tex_lock);
}

// agl's BC4 encoder draws a BC texture's blocks through an RGBA16UI view, one texel per 8-byte block (refs/nxnative
// docs/agents/04-graphics.md, Breath of the Wild and Tears of the Kingdom sections). Vulkan does it with an uncompressed
// view of a BLOCK_TEXEL_VIEW_COMPATIBLE image; EXTENDED_USAGE lets the BC image carry the COLOR_ATTACHMENT usage that
// only such views support. Whether the device takes that combination for this format and shape:
// DCC (colour compression) on Mesa's AMD driver before RDNA3 stays on for a MUTABLE_FORMAT image only when it lists
// its view formats; without a list every render target was uncompressed. smo-recomp perf/dcc-format-list (Radeon 660M,
// RADV): GPU ms per frame median 14.92 -> 13.97. A colour render target lists its own format and its sRGB / UNORM twin,
// and without a twin is not mutable; a target view of another format then uses the image's own (vk_texture_view_format).
// MKW_VK_FORMAT_LIST=0 / 1: off / on; by default on with an integrated GPU.
VkFormat vk_srgb_twin(VkFormat f) {
    static const VkFormat pairs[][2] = {
        {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB}, {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB},
        {VK_FORMAT_A8B8G8R8_UNORM_PACK32, VK_FORMAT_A8B8G8R8_SRGB_PACK32}, {VK_FORMAT_R8_UNORM, VK_FORMAT_R8_SRGB},
        {VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_SRGB}};
    for (size_t i = 0; i < sizeof pairs / sizeof *pairs; i++)
        if (pairs[i][0] == f || pairs[i][1] == f) return pairs[i][pairs[i][0] == f];
    return VK_FORMAT_UNDEFINED;
}

static int list_storage(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = cfg_get("MKW_VK_FORMAT_LIST_STORAGE");
        on = !e || !*e || atoi(e) != 0;
        if (!on) rt_log("vk: render targets with storage usage do not list their view formats (MKW_VK_FORMAT_LIST_STORAGE=0)");
    }
    return on;
}

static int format_list_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = cfg_get("MKW_VK_FORMAT_LIST");
        on = e && *e ? atoi(e) != 0 : vk_ctx()->props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
        rt_log("vk: render targets list their view formats (DCC): %s", on ? "on" : "off");
    }
    return on;
}

VkFormat vk_texture_view_format(const VkTexImage *img, VkFormat want) {
    if (!img->listed || want == img->format || want == img->twin) return want;
    static uint8_t said;
    if (!said) said = 1, rt_log("vk: a target view of format %d over an image listing %d and %d: the image's own format",
                                (int)want, (int)img->format, (int)img->twin);
    return img->format;
}

static int block_views_ok(const VkImageCreateInfo *ci) {
    VkPhysicalDeviceImageFormatInfo2 fi = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
                                           .format = ci->format, .type = ci->imageType, .tiling = ci->tiling,
                                           .usage = ci->usage, .flags = ci->flags};
    VkImageFormatProperties2 p = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
    if (vkGetPhysicalDeviceImageFormatProperties2(vk_ctx()->phys, &fi, &p) != VK_SUCCESS) return 0;
    const VkImageFormatProperties *q = &p.imageFormatProperties;
    return ci->extent.width <= q->maxExtent.width && ci->extent.height <= q->maxExtent.height &&
           ci->extent.depth <= q->maxExtent.depth && ci->mipLevels <= q->maxMipLevels &&
           ci->arrayLayers <= q->maxArrayLayers;
}

// UNDEFINED -> GENERAL; new depth / stencil images start cleared (LAS textures.c: the game copies depth
// without stencil, and the stencil of those images must not be whatever the memory held).
void init_image(Tex *e, VkCommandBuffer cb) {
    VkImageMemoryBarrier2 ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
                                .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .image = e->pub.image, .subresourceRange = {e->pub.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ib};
    vkCmdPipelineBarrier2(cb, &di);
    if (e->sparse) {
        // Tiles get zeroed memory when they are bound; a clear would erase physical pages other images share.
    } else if (e->f.depth || e->f.stencil) {
        VkClearDepthStencilValue zero = {0.0f, 0};
        VkImageSubresourceRange rg = {e->pub.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        vkCmdClearDepthStencilImage(cb, e->pub.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &rg);
        barrier(cb, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL_RW);
    } else if (vk_fix_zero() && !e->bound_to && !e->pub.mirrored && !format_compressed(e->f.vk)) {
        // GPU-only storage that got memory of its own starts at zero, as the pool it stands for (vk_memory.c
        // alloc_physical). Images aliasing pool memory are not cleared: that would erase what other textures rendered
        // there; mirrored images are uploaded from their storage on first use.
        VkClearColorValue zero = {0};
        VkImageSubresourceRange rg = {e->pub.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        vkCmdClearColorImage(cb, e->pub.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &rg);
        barrier(cb, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL_RW);
    }
}

// 1 when no ASTC block of the storage can produce alpha below 1: every block is a void-extent block with alpha
// 0xffff or uses one partition (or several of one class, CEM selector 0) with a colour endpoint mode without alpha
// (CEM 0-3, 6-9, 11). Anything else (alpha modes 4, 5, 10, 12-15, mixed classes, reserved blocks) counts as alpha;
// blocks of zeros are layout padding, and storage of only zeros (not loaded yet) is not taken as opaque. Khronos Data Format Specification 1.3, ASTC section (block mode, CEM).
#define ASTC_ALPHA_CEM (1u << 4 | 1u << 5 | 1u << 10 | 1u << 12 | 1u << 13 | 1u << 14 | 1u << 15)
static int astc_opaque(const uint8_t *p, uint64_t size) {
    int any = 0;
    static const uint16_t alpha_cem = ASTC_ALPHA_CEM;
    for (uint64_t at = 0; at + 16 <= size; at += 16) {
        uint64_t lo, hi;
        memcpy(&lo, p + at, 8), memcpy(&hi, p + at + 8, 8);
        if ((lo & 0x1ff) == 0x1fc) {   // void extent: RGBA as four u16 in the top 64 bits
            if ((hi >> 48) != 0xffff) return 0;
            continue;
        }
        if (!lo && !hi) continue;   // layout padding between rows and levels
        if (!(lo & 0x7ff)) return 0;   // reserved block mode: the error colour
        unsigned parts = (unsigned)((lo >> 11) & 3) + 1, cem;
        if (parts == 1) cem = (unsigned)((lo >> 13) & 15);
        else if (((lo >> 23) & 3) == 0) cem = (unsigned)((lo >> 25) & 15);
        else return 0;
        if (alpha_cem >> cem & 1) return 0;
        any = 1;
    }
    return any;
}

// 1 when every block of the storage decodes to alpha 255 (tex_astc.c, from each block's endpoints): the blocks
// astc_opaque takes as alpha because of their colour endpoint mode may still hold alpha 1 everywhere (728 of 768 ASTC
// images of the race stayed BC7 for their modes, bc1r; 552 of them decode to alpha 1, bca1). Decoding every texel took
// 5.4 s of the load on the replay thread (bca2). Storage is block-linear, but every 16 bytes is still one block.
static int astc_alpha_one(const uint8_t *p, uint64_t size, int bw, int bh) {
    int any = 0;
    for (uint64_t at = 0; at + 16 <= size; at += 16) {
        uint64_t lo, hi;
        memcpy(&lo, p + at, 8), memcpy(&hi, p + at + 8, 8);
        if (!lo && !hi) continue;   // layout padding; only zeros: not loaded yet, not opaque
        any = 1;
        // a single partition (or one class) without an alpha mode needs no decode (astc_opaque's test)
        if ((lo & 0x1ff) != 0x1fc && (lo & 0x7ff)) {
            unsigned parts = (unsigned)((lo >> 11) & 3) + 1, cem = 16;
            if (parts == 1) cem = (unsigned)((lo >> 13) & 15);
            else if (((lo >> 23) & 3) == 0) cem = (unsigned)((lo >> 25) & 15);
            if (cem < 16 && !(ASTC_ALPHA_CEM >> cem & 1)) continue;
        }
        if (!astc_block_alpha_one(p + at, (uint32_t)bw, (uint32_t)bh)) return 0;
    }
    return any;
}

// Texture memory made before play (MKW_VK_TEX_POOL_MIN=<blocks>, default half the VRAM, 0 off; by default on discrete
// GPUs only, a value set makes it on any): a pool of fixed 256 MiB blocks, that many of them allocated at start on a
// thread of their own, takes the textures of its memory type.
// On a GTX 1050 Ti the race load spent 79-93 % of a 2.2 s frame on the replay thread in vkAllocateMemory: VMA's
// default pools, over the driver's budget (which counts the imported host memory), give every texture its own
// allocation (VmaBlockVector::AllocatePage), and a pool whose blocks are made on demand only moved that time into
// CreateBlock (wslow3, wslow4, 06/11/2026). A pool with an explicit block size never falls back to dedicated memory;
// with its blocks made ahead the load only suballocates. An image the pool cannot hold takes the default path.
// 8 blocks: the two long frames after «VALE» 2 642 + 2 471 ms → 1 661 + 1 476 (wtp8 / wtp0); race the same (wtp8r 49
// ms, wres1 48).
#define TEX_POOL_BLOCK (256ull << 20)
static VmaPool tex_pool;              // set once its blocks exist (tex_pool_main)
static uint32_t tex_pool_type;

static void tex_pool_main(void *arg) {
    int blocks = (int)(intptr_t)arg;
    VkCtx *c = vk_ctx();
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {1024, 1024, 1}, .mipLevels = 1, .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    uint32_t type;
    VmaPool pool = NULL;
    uint64_t t0 = plat_now_ns();
    if (vmaFindMemoryTypeIndexForImageInfo(c->vma, &ci, &ai, &type) == VK_SUCCESS) {
        VmaPoolCreateInfo pi = {.memoryTypeIndex = type, .blockSize = TEX_POOL_BLOCK, .minBlockCount = (size_t)blocks};
        if (vmaCreatePool(c->vma, &pi, &pool) != VK_SUCCESS) pool = NULL;
    }
    rt_log("vk: texture pool: %s (%d blocks of %llu MiB, memory type %u, %.0f ms; MKW_VK_TEX_POOL_MIN)",
           pool ? "made" : "failed", blocks, (unsigned long long)(TEX_POOL_BLOCK >> 20), type, (plat_now_ns() - t0) / 1e6);
    if (!pool) return;
    __atomic_store_n(&tex_pool_type, type, __ATOMIC_RELAXED);
    __atomic_store_n(&tex_pool, pool, __ATOMIC_RELEASE);
}

void vk_tex_pool_start(void) {
    VkCtx *c = vk_ctx();
    const char *e = cfg_get("MKW_VK_TEX_POOL_MIN");
    // A value set by hand makes the pool on any GPU: the discrete GPUs' memory paths tried on an integrated one (the
    // AMD discrete device losses of issues #4 and #5 happened with it, on a path no integrated GPU runs).
    if (!c || (c->props.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU && !(e && *e))) return;
    VkDeviceSize vram = 0;
    for (uint32_t h = 0; h < c->mem_props.memoryHeapCount; h++)
        if (c->mem_props.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT && c->mem_props.memoryHeaps[h].size > vram)
            vram = c->mem_props.memoryHeaps[h].size;
    int blocks = e && *e ? atoi(e) : (int)(vram / 2 / TEX_POOL_BLOCK);   // default: half of the largest VRAM heap
    if (blocks <= 0) return;
    if (!plat_thread_spawn(tex_pool_main, (void *)(intptr_t)blocks, "vk tex pool", 0)) rt_log("vk: texture pool: no thread");
}

VkResult vma_image(VkCtx *c, const VkImageCreateInfo *ci, const VmaAllocationCreateInfo *ai, VkImage *img,
                   VmaAllocation *alloc) {
    VmaPool pool = __atomic_load_n(&tex_pool, __ATOMIC_ACQUIRE);
    uint32_t type;
    if (pool && !ai->pool && vmaFindMemoryTypeIndexForImageInfo(c->vma, ci, ai, &type) == VK_SUCCESS &&
        type == __atomic_load_n(&tex_pool_type, __ATOMIC_RELAXED)) {
        VmaAllocationCreateInfo pa = *ai;
        pa.pool = pool;
        if (vmaCreateImage(c->vma, ci, &pa, img, alloc, NULL) == VK_SUCCESS) return VK_SUCCESS;
    }
    return vmaCreateImage(c->vma, ci, ai, img, alloc, NULL);
}

// defer_init: the caller records the image's first transition (init_image or alias_copy).

// The memory an image of ci takes (0 if it cannot be made).
static VkDeviceSize image_bytes(VkCtx *c, const VkImageCreateInfo *ci) {
    VkImage probe;
    if (vkCreateImage(c->device, ci, NULL, &probe) != VK_SUCCESS) return 0;
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(c->device, probe, &mr);
    vkDestroyImage(c->device, probe, NULL);
    return mr.size;
}

Tex *create_tex(const tex_t *t, const TexKey *k, int defer_init) {
    static volatile int32_t n_unmapped, n_unsupported, n_failed, n_unbound;
    VkCtx *c = vk_ctx();
    const nvn_texdesc *d = &t->d;
    if (!c || d->target == NVN_TEXTURE_TARGET_BUFFER) return NULL;
    TexVkFormat f;
    if (!tex_vk_format(d->format, &f)) {
        log_limited(&n_unmapped, "texture format %#x has no Vulkan format: %s", d->format, f.reason);
        return NULL;
    }
    // Small ASTC textures stay RGBA8: they are mostly data (noise, distortion, lookup tables) that a lossy BC7 mode 6
    // turns into visible noise (run116: two 32x32 repeat-wrapped inputs of the world map background), and they hold
    // little memory. Over 256x256 at level 0 they are re-encoded (vk_astc_bc7).
    if (f.astc_bc7 && (uint64_t)d->width * d->height < 256u * 256u) {
        f.astc_bc7 = 0;
        f.vk = f.astc_srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    }
    // ASTC without alpha in any block becomes BC1 (half of BC7): run-race11 held 1 136 MiB of BC7 in a 3 GB heap and
    // the race's targets did not fit. Only storage the CPU can read (the scan), under 4096 texels a side (the 8192x8192
    // ASTC textures are copy sources of the sparse arrays, which must stay BC7 to match them: vk_exec_xfer.c).
    // MKW_ASTC_BC1 (default 1; 0 keeps BC7). On by default since 08/11/2026: the GPU the same on the 660M with 230-285
    // MiB less of textures (lb0a-lb1b), the 1920x1080 scene 17.6-18.3 -> 15.7 ms on a GTX 1050 Ti (wbc*, wms*), the
    // image 0.0039 -> 0.0063 from the game's (bca5), no GPU fault at the default 768 MiB of tiles. run-race41 hung
    // the GPU with it and MKW_SPARSE_MIB=1024 (31/10/2026).
    static int bc1_on = -1;
    if (bc1_on < 0) bc1_on = !cfg_get("MKW_ASTC_BC1") || !*cfg_get("MKW_ASTC_BC1") || atoi(cfg_get("MKW_ASTC_BC1"));
    if (f.astc_bc7 && bc1_on) {
        // why an image stays BC7, counted (bc1-t, 08/11/2026: texture memory as without BC1)
        static volatile int32_t why[6], alpha_one;   // BC1, 4096 or more, no CPU storage, GPU-only, sparse, alpha
        static volatile int64_t alpha_ns;            // the decodes' time (the replay thread's, at image creation)
        int w = d->width >= 4096 || d->height >= 4096 ? 1 : !t->mem ? 2 : nvn_gpu_only(k->addr) ? 3
              : d->flags & NVN_TEXTURE_FLAGS_SPARSE ? 4 : !astc_opaque(t->mem, t->L.size) ? 5 : 0;
        int bw = 0, bh = 0;
        uint64_t t0 = plat_now_ns();
        if (w == 5 && nvn_fmt_bpb(d->format, &bw, &bh) && astc_alpha_one(t->mem, t->L.size, bw, bh))
            plat_inc32(&alpha_one), w = 0;   // alpha modes, alpha 1 decoded: BC1 too (bca1: 553 of the 728)
        __atomic_add_fetch(&alpha_ns, (int64_t)(plat_now_ns() - t0), __ATOMIC_RELAXED);
        if (!w) {
            f.astc_bc7 = 2;
            f.vk = f.astc_srgb ? VK_FORMAT_BC1_RGB_SRGB_BLOCK : VK_FORMAT_BC1_RGB_UNORM_BLOCK;
        }
        plat_inc32(&why[w]);
        int32_t all = 0;
        for (int i = 0; i < 6; i++) all += why[i];
        if (all % 256 == 0)
            rt_log("vk: ASTC BC1 (MKW_ASTC_BC1): %d of %d images; BC7 for 4096+ %d, no CPU storage %d, GPU-only %d, "
                   "sparse %d, alpha %d (BC1 for alpha 1 decoded in every texel %d, %.0f ms decoding)", why[0], all,
                   why[1], why[2], why[3], why[4], why[5], alpha_one, alpha_ns / 1e6);
    }
    if (!tex_vk_format_supported(&f)) {
        log_limited(&n_unsupported, "texture format %#x: VkFormat %d lacks features %#x", d->format, (int)f.vk, (unsigned)f.need);
        return NULL;
    }
    nvn_texlayout L;
    nvn_tex_layout(d, &L);
    uint32_t w = umax(d->width, 1), h = umax(d->height, 1), depth = 1, layers = 1;
    VkImageType type = VK_IMAGE_TYPE_2D;
    switch (d->target) {
    case NVN_TEXTURE_TARGET_1D: type = VK_IMAGE_TYPE_1D; h = 1; break;
    case NVN_TEXTURE_TARGET_1D_ARRAY: type = VK_IMAGE_TYPE_1D; h = 1; layers = L.layers; break;
    case NVN_TEXTURE_TARGET_3D: type = VK_IMAGE_TYPE_3D; depth = umax(d->depth, 1); break;
    case NVN_TEXTURE_TARGET_2D_ARRAY: case NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY:
    case NVN_TEXTURE_TARGET_CUBEMAP: case NVN_TEXTURE_TARGET_CUBEMAP_ARRAY: layers = L.layers; break;
    default: break;
    }
    uint32_t levels = umax(d->levels, 1), most = 1;
    for (uint32_t s = umax(umax(w, h), depth); s > 1; s >>= 1) most++;
    if (levels > most) levels = most;
    uint32_t samples = d->samples > 1 ? (uint32_t)d->samples : 1;
    if (samples > 1) levels = 1;
    VkFormatFeatureFlags ff = features_of(f.vk);
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    int ds = f.depth || f.stencil;
    if (ds && (ff & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (!ds && (ff & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    // Storage usage whenever the format allows it, not only for IMAGE descriptors: one image serves every descriptor
    // of its storage (KEY_FREE_FLAGS), and the first one created may be a sampled view. Possible cost: RADV may keep
    // such images without DCC (not measured yet).
    if (!ds && (ff & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) && samples == 1)
        usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    VkImageCreateFlags flags = ds ? 0 : VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;   // format-reinterpreting views
    if (type == VK_IMAGE_TYPE_2D && layers >= 6 && w == h && samples == 1) flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    // NVN renders into a 3D texture slice by slice (a view's min layer is the depth plane, nvn_mtl.m), e.g. the
    // grading LUT of the menu background blur. Vulkan needs a 2D view of the slice for that.
    if (type == VK_IMAGE_TYPE_3D && (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = flags, .imageType = type, .format = f.vk,
                            .extent = {w, h, depth}, .mipLevels = levels, .arrayLayers = layers,
                            .samples = (VkSampleCountFlagBits)samples, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    int bw, bh, block_views = 0;
    // Not for a sparse texture of a virtual pool (vk_sparse.c): RADV has no sparse blocks for a BC image with
    // COLOR_ATTACHMENT usage (run-sp2: BC6H), and the game fills those with copies.
    int sparse_cand = (d->flags & NVN_TEXTURE_FLAGS_SPARSE) && nvn_gpu_only(k->addr);
    if (!ds && samples == 1 && f.vk == f.ideal && !sparse_cand && nvn_fmt_bpb(d->format, &bw, &bh) && bw == 4 && bh == 4) {
        VkImageCreateInfo bc = ci;   // BC formats: renderable through views of their blocks (block_views_ok)
        bc.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        bc.flags |= VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
        if (type == VK_IMAGE_TYPE_3D) bc.flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
        if (block_views_ok(&bc)) ci = bc, usage = bc.usage, flags = bc.flags, block_views = 1;
    }
    // Feedback loops (vk.h VkCtx.feedback): every attachable image may be sampled by a draw of a pass that renders it,
    // so it gets the usage now; adding it when a loop shows up would mean recreating the image (that lost the
    // grass tile maps once).
    int fb_usage = c->feedback == VK_FEEDBACK_LAYOUT && !block_views &&
                   (usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT));
    if (fb_usage) ci.usage = usage |= VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;
    VkFormat listed[2] = {f.vk, vk_srgb_twin(f.vk)};
    VkImageFormatListCreateInfo fl = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, .pNext = ci.pNext,
                                      .viewFormatCount = listed[1] ? 2u : 1u, .pViewFormats = listed};
    // Storage images list too (MKW_VK_FORMAT_LIST_STORAGE, default 1): without a list RADV keeps them without DCC. Race,
    // runs alternated: GPU 19.71 / 20.25 -> 19.13 / 18.96 ms (dc-a*, dc-b*). They lose the R32_UINT view of 32-bit
    // texels for image atomics (vk_exec_view.c get_view warns once; none in those runs).
    int list = (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) && (!(usage & VK_IMAGE_USAGE_STORAGE_BIT) || list_storage()) &&
               !block_views && (ci.flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) && format_list_on();
    if (list && listed[1]) ci.pNext = &fl;
    else if (list) ci.flags &= ~(VkImageCreateFlags)VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    Tex *e = calloc(1, sizeof *e);
    if (!e) rt_fatal("vk: out of memory for a texture");
    e->key = *k;
    e->checked = e->seen = submit_epoch;   // new: not idle (evict_idle)
    e->t = *t;
    e->f = f;
    e->pub.addr = k->addr;
    VkImage img = VK_NULL_HANDLE;
    if (vk_sparse_create(&ci, d, k->addr, L.size, &img)) e->sparse = 1;
    else if (sparse_cand) {
        e->sparse_fallback = 1;
        log_limited(&n_unbound, "sparse texture %#llx (%ux%u fmt %#x, VkFormat %d usage %#x, %u levels, %u layers) "
                    "has no standard sparse blocks; VMA-backed image instead",
                    (unsigned long long)k->addr, w, h, d->format, (int)ci.format, ci.usage, ci.mipLevels, ci.arrayLayers);
    } else if (nvn_gpu_only(k->addr)) {   // storage in a physical or virtual pool: alias the pool memory (section 3.2)
        if (vkCreateImage(c->device, &ci, NULL, &img) == VK_SUCCESS && !bind_pool_memory(c, e, img)) {
            vkDestroyImage(c->device, img, NULL);
            img = VK_NULL_HANDLE;
            log_limited(&n_unbound, "texture %#llx (%ux%u fmt %#x, %u levels, %u layers) cannot alias its pool memory; "
                        "own memory instead", (unsigned long long)k->addr, w, h, d->format, ci.mipLevels, ci.arrayLayers);
        }
    }
    if (!img) {
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VkResult r = vma_image(c, &ci, &ai, &img, &e->alloc);
        // Out of memory: released entries kept for a re-use go first, oldest first, then the retry (run115: 1 230
        // released entries held 596 MiB while new textures failed). Their images go to the deferred list; what the
        // GPU finished with is freed by vk_mem_collect.
        for (int round = 0; r == VK_ERROR_OUT_OF_DEVICE_MEMORY && rel_head && round < 8; round++) {
            VkDeviceSize need = image_bytes(c, &ci), freed = 0;
            while (rel_head && freed < 2 * need + (16ull << 20)) {
                VmaAllocationInfo vi = {0};
                if (rel_head->alloc) vmaGetAllocationInfo(c->vma, rel_head->alloc, &vi);
                freed += vi.size;
                destroy_idle(rel_head);
            }
            vk_mem_collect();
            r = vma_image(c, &ci, &ai, &img, &e->alloc);
        }
        if (r == VK_ERROR_OUT_OF_DEVICE_MEMORY) r = evict_idle(c, &ci, &ai, &img, &e->alloc);
        // Last: what the releases and evictions above left waiting for the open upload batch or the GPU (tt6a,
        // 09/11/2026: 23 images not made at the time trial's load right after 256 evictions).
        if (r == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
            vk_mem_relieve(2 * image_bytes(c, &ci) + (16ull << 20), "an image");
            r = vma_image(c, &ci, &ai, &img, &e->alloc);
        }
        if (r != VK_SUCCESS) {
            // Once per shape, up to 64: one texture retried every frame used up the shared limit (run107: a 8192x8192x68
            // array 16 times) and hid the render targets that failed after it.
            static struct { int32_t fmt; uint32_t w, h, layers; } seen[64];
            static int nseen;
            int k = 0;
            while (k < nseen && !(seen[k].fmt == d->format && seen[k].w == w && seen[k].h == h && seen[k].layers == layers)) k++;
            if (k == nseen && nseen < 64) {
                seen[nseen++] = (typeof(seen[0])){d->format, w, h, layers};
                rt_log("vk: image creation failed (%d): nvn fmt %#x %ux%ux%u target %d levels %u layers %u", (int)r,
                       d->format, w, h, depth, d->target, levels, layers);
                if (nseen <= 4) vram_log_now();
            }
            plat_inc32(&n_failed);
            free(e);
            return NULL;
        }
    }
    if (e->alloc) {
        VmaAllocationInfo vi;
        vmaGetAllocationInfo(c->vma, e->alloc, &vi);
        if (vi.size >= (64ull << 20))   // what fills the heap (run110: 1 571 MiB in images over 64 MiB)
            rt_log("vk: large image %.0f MiB: addr %#llx nvn fmt %#x %ux%ux%u target %d levels %u layers %u flags %#x%s",
                   vi.size / 1048576.0, (unsigned long long)k->addr, d->format, w, h, depth, d->target, levels, layers,
                   d->flags, nvn_gpu_only(k->addr) ? " (GPU pool, not aliased)" : "");
    }
    VkTexImage *p = &e->pub;
    p->image = img;
    p->format = f.vk;
    p->type = type;
    p->usage = usage;
    p->aspect = aspect_of(&f);
    p->width = w, p->height = h, p->depth = depth, p->levels = levels, p->layers = layers, p->samples = samples;
    p->view_type = type == VK_IMAGE_TYPE_1D ? (d->target == NVN_TEXTURE_TARGET_1D_ARRAY ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D)
                 : type == VK_IMAGE_TYPE_3D ? VK_IMAGE_VIEW_TYPE_3D
                 : d->target == NVN_TEXTURE_TARGET_CUBEMAP && (flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) ? VK_IMAGE_VIEW_TYPE_CUBE
                 : d->target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY && (flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY
                 : layers > 1 || d->target == NVN_TEXTURE_TARGET_2D_ARRAY || d->target == NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY
                     ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    p->alpha_one = f.alpha_one;
    memcpy(p->chan, f.chan, sizeof p->chan);
    p->swz[0] = (uint8_t)d->swizzle_r, p->swz[1] = (uint8_t)d->swizzle_g;
    p->swz[2] = (uint8_t)d->swizzle_b, p->swz[3] = (uint8_t)d->swizzle_a;
    if (!(p->swz[0] | p->swz[1] | p->swz[2] | p->swz[3]))   // descriptors built without nvnTextureBuilderSetDefaults
        p->swz[0] = NVN_TEXTURE_SWIZZLE_R, p->swz[1] = NVN_TEXTURE_SWIZZLE_G, p->swz[2] = NVN_TEXTURE_SWIZZLE_B,
        p->swz[3] = NVN_TEXTURE_SWIZZLE_A;
    p->mirrored = (uint8_t)tex_mirrored(t);
    p->bound = e->bound_to != NULL;
    p->block_views = (uint8_t)block_views;
    p->listed = (uint8_t)list, p->twin = listed[1];
    p->fb_usage = (uint8_t)fb_usage;

    if (!defer_init) {
        init_image(e, vk_upload_begin());
        vk_upload_end();
    }
    return e;
}

