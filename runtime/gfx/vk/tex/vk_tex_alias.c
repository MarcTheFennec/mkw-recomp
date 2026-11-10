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

// ---------------------------------------------------------------- aliases (MKW_TEX_ALIAS)
// Bytes per block and block size of the image's VkFormat (ASTC decodes to RGBA8, RGB expands to RGBA); 0 when the
// VkFormat substitutes another one of unknown size.
static int img_block(const Tex *e, int *bw, int *bh) {
    if (e->f.astc_bc7) { *bw = *bh = 4; return e->f.astc_bc7 == 2 ? 8 : 16; }   // re-encoded as BC7 or BC1 (vk_astc_bc7)
    if (e->f.astc_bw) { *bw = *bh = 1; return 4; }
    int b = nvn_fmt_bpb(e->t.d.format, bw, bh);
    if (e->f.expand_rgb) { *bw = *bh = 1; return b / 3 * 4; }
    return e->f.vk == e->f.ideal ? b : 0;
}

// The new entry e can take src's content texel for texel: colour, one sample, the same shape and storage layout, and
// image formats of one block size.
static int alias_compatible(const Tex *e, const Tex *src) {
    const VkTexImage *p = &e->pub, *q = &src->pub;
    if (e->f.depth || e->f.stencil || src->f.depth || src->f.stencil || p->samples > 1 || q->samples > 1) return 0;
    if (p->type != q->type || p->width != q->width || p->height != q->height || p->depth != q->depth) return 0;
    if (e->t.L.size != src->t.L.size || nvn_tex_is_linear(&e->t.d) != nvn_tex_is_linear(&src->t.d)) return 0;
    int ew, eh, sw, sh, eb = img_block(e, &ew, &eh), sb = img_block(src, &sw, &sh);
    return eb && eb == sb && ew == sw && eh == sh;
}

// src holds content the storage bytes do not: the GPU wrote it and, for a mirrored texture, it was not written back
// and the CPU has not rewritten the storage since (the new entry's upload then reads the newer bytes).
static int alias_has_gpu_content(const Tex *src) {
    if (!src->pub.gpu_owned) return 0;
    if (!src->pub.mirrored) return 1;
    uint32_t now;
    return src->gpu_dirty && !storage_rewritten(src, src->gpu_gen, &now);
}

// Maxwell reads the same bytes through both descriptors: src's content goes into the new image e at the replay's
// current position, before any later command of the submission uses e. Through a buffer, since bound images share
// their memory and image copies may not overlap; e leaves UNDEFINED between the two halves, so its first transition
// cannot clobber src's bytes before they are read. 0 when no replay is recording (e is then not initialised yet).
static int alias_copy(Tex *e, const Tex *src) {
    int reopen;
    VkCommandBuffer cb = vk_exec_inline_begin(&reopen);
    if (!cb) return 0;
    int bw, bh, bpb = img_block(e, &bw, &bh);
    uint32_t levels = e->pub.levels < src->pub.levels ? e->pub.levels : src->pub.levels;
    uint32_t layers = e->pub.layers < src->pub.layers ? e->pub.layers : src->pub.layers, n = levels * layers, k = 0;
    VkBufferImageCopy *rg = calloc(n, sizeof *rg);
    if (!rg) rt_fatal("vk: out of memory for an alias copy");
    VkDeviceSize total = 0;
    for (uint32_t lv = 0; lv < levels; lv++)
        for (uint32_t ly = 0; ly < layers; ly++, k++) {
            uint32_t w = mip(e->pub.width, lv), h = mip(e->pub.height, lv), z = mip(e->pub.depth, lv);
            rg[k] = (VkBufferImageCopy){.bufferOffset = total, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lv, ly, 1},
                                        .imageExtent = {w, h, z}};
            total += ((VkDeviceSize)(w + bw - 1) / bw * ((h + bh - 1) / bh) * z * bpb + 15) & ~15ull;
        }
    VkBuffer buf = vk_exec_inline_buffer(total);
    if (!buf) {
        free(rg);
        vk_exec_inline_end(reopen);
        return 0;
    }
    barrier(cb, ALL_RW, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    vkCmdCopyImageToBuffer(cb, src->pub.image, VK_IMAGE_LAYOUT_GENERAL, buf, n, rg);
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                           .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                           .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT};
    VkImageMemoryBarrier2 ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                                .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT, .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .image = e->pub.image,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb,
                           .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ib};
    vkCmdPipelineBarrier2(cb, &di);
    vkCmdCopyBufferToImage(cb, buf, e->pub.image, VK_IMAGE_LAYOUT_GENERAL, n, rg);
    barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL_RW);
    vk_exec_inline_end(reopen);
    free(rg);
    return 1;
}

// Copies src's content into e on the upload batch when no inline replay command buffer is open (e.g. during
// MapVirtual or outside vk_exec_record). Only used when e has its own VMA-backed memory (sparse_fallback).
static int upload_alias_copy(Tex *e, const Tex *src) {
    if (alias_copy(e, src)) return 1;
    if (e->bound_to || src->bound_to) return 0;
    int bw, bh, bpb = img_block(e, &bw, &bh);
    if (!bpb) return 0;
    uint32_t levels = e->pub.levels < src->pub.levels ? e->pub.levels : src->pub.levels;
    uint32_t layers = e->pub.layers < src->pub.layers ? e->pub.layers : src->pub.layers, n = levels * layers, k = 0;
    VkImageCopy *rg = calloc(n, sizeof *rg);
    if (!rg) return 0;
    for (uint32_t lv = 0; lv < levels; lv++)
        for (uint32_t ly = 0; ly < layers; ly++, k++) {
            uint32_t w = mip(e->pub.width, lv), h = mip(e->pub.height, lv), z = mip(e->pub.depth, lv);
            rg[k] = (VkImageCopy){.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lv, ly, 1},
                                  .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lv, ly, 1},
                                  .extent = {w, h, z}};
        }
    VkCommandBuffer cb = vk_upload_begin();
    barrier(cb, ALL_RW, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
    vkCmdCopyImage(cb, src->pub.image, VK_IMAGE_LAYOUT_GENERAL, e->pub.image, VK_IMAGE_LAYOUT_GENERAL, n, rg);
    barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL_RW);
    vk_upload_end();
    free(rg);
    return 1;
}

// Copies mapped physical pool pages in [lo, hi) into a non-sparse VMA-backed fallback image e.
static void remap_virtual_entry(Tex *e, uint64_t lo, uint64_t hi) {
    uint64_t pa = 0;
    if (vk_mem_virt_phys(e->pub.addr, &pa)) {
        Tex *src = NULL;
        for (Tex *p = nvn_hm_get(&by_addr, &pa); p; p = p->next_at) {
            if (p == e || p->released) continue;
            if ((!src || (same_but_swizzle(&p->key, &e->key) && !same_but_swizzle(&src->key, &e->key))) &&
                alias_compatible(e, p) && alias_has_gpu_content(p))
                src = p;
        }
        if (src && upload_alias_copy(e, src)) {
            n_alias_copied++;
            e->pub.gpu_owned = 1;
            return;
        }
    }
    if (e->pub.gpu_owned) return;
    int bw, bh, bpb = img_block(e, &bw, &bh);
    int32_t lay[4];
    nvn_sparse_tile_layout(&e->t.d, lay);
    uint32_t ntw = (uint32_t)lay[1], nth = (uint32_t)lay[2], nnvn = (uint32_t)lay[0];
    if (!bpb || !ntw || !nth || nnvn > 16 ||
        (uint64_t)(ntw / (uint32_t)bw) * (nth / (uint32_t)bh) * (uint32_t)bpb != NVN_VPOOL_PAGE)
        return;
    uint32_t tiles_x[16], tiles_y[16], tile0[16], stride = 0;
    for (uint32_t l = 0; l < nnvn; l++) {
        uint32_t lw = mip(e->pub.width, l), lh = mip(e->pub.height, l);
        tiles_x[l] = (lw + ntw - 1) / ntw, tiles_y[l] = (lh + nth - 1) / nth, tile0[l] = stride;
        stride += tiles_x[l] * tiles_y[l];
    }
    if (!stride) return;
    if (lo < e->pub.addr) lo = e->pub.addr;
    if (hi > e->pub.addr + e->t.L.size) hi = e->pub.addr + e->t.L.size;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    for (uint64_t a = lo & ~(uint64_t)(NVN_VPOOL_PAGE - 1); a < hi; a += NVN_VPOOL_PAGE) {
        uint64_t p = (a - e->pub.addr) / NVN_VPOOL_PAGE;
        uint32_t layer = (uint32_t)(p / stride), rel = (uint32_t)(p % stride), l = 0;
        if (layer >= e->pub.layers) continue;
        while (l + 1 < nnvn && rel >= tile0[l + 1]) l++;
        if (l >= e->pub.levels) continue;
        VkMemSpan sp;
        if (!vk_mem_resolve(a, NVN_VPOOL_PAGE, &sp) || !sp.buffer) continue;
        uint32_t t = rel - tile0[l], tx = t % tiles_x[l], ty = t / tiles_x[l];
        uint32_t lw = mip(e->pub.width, l), lh = mip(e->pub.height, l);
        uint32_t x = tx * ntw, y = ty * nth;
        if (x >= lw || y >= lh) continue;
        if (!cb) {
            cb = vk_upload_begin();
            barrier(cb, ALL_RW, VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
        }
        VkBufferImageCopy rg = {
            .bufferOffset = sp.offset,
            .bufferRowLength = ntw,
            .bufferImageHeight = nth,
            .imageSubresource = {e->pub.aspect, l, layer, 1},
            .imageOffset = {(int32_t)x, (int32_t)y, 0},
            .imageExtent = {lw - x < ntw ? lw - x : ntw, lh - y < nth ? lh - y : nth, 1},
        };
        vkCmdCopyBufferToImage(cb, sp.buffer, e->pub.image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
    }
    if (cb) {
        barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL_RW);
        vk_upload_end();
    }
}

void vk_texture_remap_virtual(uint64_t lo, uint64_t size) {
    if (!__atomic_load_n(&tex_ready, __ATOMIC_ACQUIRE)) return;
    lock();
    uint64_t hi = lo + size;
    for (uint32_t i = 0; i < by_key.cap; i++) {
        Tex *e = by_key.vals ? by_key.vals[i] : NULL;
        if (!e || e->released || !e->sparse_fallback) continue;
        if (lo < e->pub.addr + e->t.L.size && e->pub.addr < hi) {
            remap_virtual_entry(e, lo, hi);
            tex_gen_bump(e->key.addr);
        }
    }
    unlock();
}

// The entries at a storage address, chained by next_at (ui/glyphs/glyphs.c re-uploads all of them). Texture lock held.
Tex *entries_at(uint64_t addr) { return nvn_hm_get(&by_addr, &addr); }

Tex *entry_of(const tex_t *t) {
    TexKey k;
    make_key(t, &k);
    Tex *e = nvn_hm_get(&by_key, &k);
    if (!e) {
        if (tex_alias < 0) {
            tex_alias = cfg_bool("MKW_TEX_ALIAS");
            rt_log("vk: texture aliases: %s", tex_alias ? "a new image takes the live image's content" : "a new image starts empty");
        }
        if (!(e = create_tex(t, &k, tex_alias))) return NULL;
        perf_count(PERF_CNT_TEX_NEW);
        // released entries of another descriptor at this address are replaced by the new one; a live one holding
        // GPU-written content is the alias source, preferring the one that differs only in swizzle (same_but_swizzle)
        int alias = 0;
        Tex *src = NULL;
        for (Tex *p = nvn_hm_get(&by_addr, &k.addr), *nx; p; p = nx) {
            nx = p->next_at;
            if (p->released) { destroy_idle(p); continue; }
            alias = 1;
            if (tex_alias && (!src || (same_but_swizzle(&p->key, &k) && !same_but_swizzle(&src->key, &k))) &&
                alias_compatible(e, p) && alias_has_gpu_content(p))
                src = p;
        }
        uint64_t pa = 0;
        if (!src && e->sparse_fallback && vk_mem_virt_phys(k.addr, &pa)) {
            for (Tex *p = nvn_hm_get(&by_addr, &pa); p; p = p->next_at) {
                if (p->released) continue;
                alias = 1;
                if (tex_alias && (!src || (same_but_swizzle(&p->key, &k) && !same_but_swizzle(&src->key, &k))) &&
                    alias_compatible(e, p) && alias_has_gpu_content(p))
                    src = p;
            }
        }
        if (alias) perf_count(PERF_CNT_TEX_ALIAS);
        if (tex_alias) {
            if (src && (alias_copy(e, src) || (e->sparse_fallback && upload_alias_copy(e, src)))) {
                n_alias_copied++;
                if (e->pub.mirrored || e->sparse_fallback) {   // the content is src's GPU render, newer than the storage bytes
                    if (e->pub.mirrored) vk_mem_changed(e->pub.addr, e->t.L.size, 0, &e->wgen);
                    e->uploaded = 1;
                    e->pub.gpu_owned = 1;
                }
            } else {
                init_image(e, vk_upload_begin());
                vk_upload_end();
                if (e->sparse_fallback) remap_virtual_entry(e, e->pub.addr, e->pub.addr + e->t.L.size);
                if (alias) {
                    if (e->pub.mirrored) n_alias_upload++;
                    else n_alias_uninit++;
                }
            }
        } else if (e->sparse_fallback) {
            remap_virtual_entry(e, e->pub.addr, e->pub.addr + e->t.L.size);
        }
        e->next_at = nvn_hm_get(&by_addr, &k.addr);
        nvn_hm_put(&by_addr, &k.addr, e);
        nvn_hm_put(&by_key, &k, e);
        tex_gen_bump(k.addr);
    }
    if (e->released) e->released = 0, nreleased--, rel_unlink(e), tex_gen_bump(k.addr);
    e->t = *t;
    return e;
}

// vk_dump: upload bookkeeping of an image vk_texture_get / vk_texture_by_id returned. pages_written: whether its
// storage pages were written since the last check (1, 0, -1 not tracked); the query leaves that check's state alone.
void vk_texture_debug(const VkTexImage *img, uint32_t *uploads, int *last_change, int *pages_written) {
    lock();
    const Tex *e = (const Tex *)img;   // pub is the first member
    uint32_t now;
    *uploads = e->n_up;
    *last_change = e->last_ch;
    *pages_written = e->pub.mirrored && e->uploaded ? vk_mem_changed(e->pub.addr, e->t.L.size, e->wgen, &now) : 0;
    unlock();
}

const VkTexImage *vk_texture_get(const tex_t *t) {
    if (!t || !t->mem) return NULL;
    lock();
    Tex *e = entry_of(t);
    if (e) make_fresh(e), e->seen = submit_epoch;
    if (e && vk_guard_on()) vk_guard_note(&e->use);
    unlock();
    return e ? &e->pub : NULL;
}

// Small render targets the CPU reads without NVN's invalidate (vk_texture_write_back_small); small_pending: one may be
// dirty.
int small_pending;

// Without creating an entry (entry_of would: a capture asked about every registered texture and made images for
// thousands nobody used).
int vk_texture_capture_state(const tex_t *t, uint32_t recent) {
    if (!t || !t->mem) return 0;
    TexKey k;
    make_key(t, &k);
    lock();
    Tex *e = nvn_hm_get(&by_key, &k);
    int r = 0;
    if (e && !e->released && e->pub.image) {
        if (e->pub.gpu_owned) r |= VK_TEXCAP_GPU;
        if (vk_sparse_is(e->pub.image)) r |= VK_TEXCAP_SPARSE;
        if (e->checked && (uint32_t)(submit_epoch - e->checked) <= recent) r |= VK_TEXCAP_RECENT;
    }
    unlock();
    return r;
}

void vk_texture_gpu_written(const tex_t *t) {
    if (!t || !t->mem) return;
    if (tracing((uintptr_t)t->mem)) {   // with the replay function that wrote it (copies, clears, render targets)
        // the caller as an offset in the executable (addr2line -f -e build/mkw)
        uintptr_t off = (uintptr_t)__builtin_return_address(0) - plat_exe_base();
        rt_log("vk: texture %#llx %ux%ux%u fmt %#x written on the GPU, caller build/mkw+%#llx", (unsigned long long)trace_addr,
               t->d.width, t->d.height, t->d.depth, t->d.format, (unsigned long long)off);
    }
    vk_exec_frame_written(t);
    lock();
    Tex *e = entry_of(t);
    if (e) {
        make_fresh(e);   // a partial render keeps what the storage held before
        e->pub.gpu_owned = 1;
        uint64_t f = vk_exec_recording_fence();
        if (f > e->wr_fence) e->wr_fence = f;
        if (e->pub.mirrored) {
            e->gpu_dirty = 1;
            storage_rewritten(e, 0, &e->gpu_gen);   // CPU writes up to here are older than this render
            e->gpu_seq = ++gpu_writes;
            e->gpu_fence = vk_exec_recording_fence();
            if (e->t.L.size <= WB_SMALL) __atomic_store_n(&small_pending, 1, __ATOMIC_RELEASE);
            own_add(e);
        }
    }
    unlock();
}

void vk_texture_release(const tex_t *t) {
    if (!t || !t->mem) return;
    TexKey k;
    make_key(t, &k);
    lock();
    Tex *e = nvn_hm_get(&by_key, &k);
    if (e && !e->released) {
        e->released = 1;
        nreleased++;
        rel_append(e);
        tex_gen_bump(k.addr);
        sweep_released();
    }
    unlock();
}

void vk_texture_range_gone(uint64_t lo, uint64_t size, VmaAllocation alloc) {
    lock();
    Tex **list = malloc(sizeof *list * (size_t)(by_key.n + 1));
    int n = 0;
    for (uint32_t i = 0; i < by_key.cap; i++) {
        Tex *e = by_key.vals[i];
        if (e && ((e->key.addr >= lo && e->key.addr - lo < size) || (alloc && e->bound_to == alloc))) list[n++] = e;
    }
    for (int i = 0; i < n; i++) destroy_tex(list[i]);
    free(list);
    unlock();
}

