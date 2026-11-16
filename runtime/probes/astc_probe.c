// --probe-astc: the compute ASTC decoder (vk_astc.c, shaders/astc_decode.comp) against astc_decode_ldr, bit for bit,
// for every block footprint, linear and sRGB. Synthetic blocks only (no game files): valid void extents, invalid and
// HDR void extents, random blocks kept by what they exercise (1-4 partitions, dual plane with 1-3 partitions, every
// weight grid and quantization the random modes reach), then plain random blocks (mostly error blocks, HDR endpoint
// modes among them). Each case decodes two regions into two mip levels of an RGBA8 image with cropped edges, and
// compares both the decoder's texels (readback) and what the image holds afterwards. Also times one 2048x2048 texture
// on the CPU (astc_decode_ldr_mt) and on the GPU (wall time of the upload batch).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/platform.h"
#include "host.h"
#include "tex.h"
#include "vk.h"

static uint64_t g_rng = 0x2545F4914F6CDD1Dull;
static uint64_t rnd64(void) {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    return g_rng;
}

static const uint8_t dims[14][2] = {{4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6},
                                    {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};

// Buckets of valid blocks: partitions 1-4 single plane, then dual plane with 1-3 partitions.
enum { NB = 7, PER_BUCKET = 48 };
static const char *const bucket_name[NB] = {"1p", "2p", "3p", "4p", "dual 1p", "dual 2p", "dual 3p"};
static uint64_t g_bucket[NB], g_void, g_err, g_blocks, g_texels;

static int dual_bit(uint32_t m) {
    if (!(m & 3) && (m >> 7 & 3) == 2) return 0;   // the 6..10 x 6..10 modes have no dual plane
    return (int)(m >> 10 & 1);
}

static void void_block(uint8_t b[16], int kind) {
    // kind 0: valid LDR extent (random coordinates s0 < s1, t0 < t1), 1: all-ones extent, 2: s0 >= s1 (error),
    // 3: HDR (error), 4: reserved bits not 11 (error)
    uint64_t lo = 0x1FC | 3ull << 10, s0 = rnd64() & 0xFFF, s1 = s0 + 1 + (rnd64() & 0xFFF), t0 = rnd64() & 0xFFF,
             t1 = t0 + 1 + (rnd64() & 0xFFF);
    if (s1 > 0x1FFF) s1 = 0x1FFF;
    if (t1 > 0x1FFF) t1 = 0x1FFF;
    if (kind == 1) s0 = s1 = t0 = t1 = 0x1FFF;
    if (kind == 2) { uint64_t x = s0; s0 = s1; s1 = x; }
    lo |= s0 << 12 | s1 << 25 | t0 << 38 | t1 << 51;
    if (kind == 3) lo |= 0x200;
    if (kind == 4) lo &= ~(1ull << 11);
    uint64_t hi = rnd64();
    memcpy(b, &lo, 8), memcpy(b + 8, &hi, 8);
}

// n blocks for footprint bw x bh: the buckets first, void extents, then random.
static void make_blocks(uint8_t *blk, size_t n, uint32_t bw, uint32_t bh) {
    size_t k = 0;
    uint8_t tmp[12 * 12 * 4];
    int have[NB] = {0};
    for (int tries = 0; tries < 400000 && k < n; tries++) {
        uint64_t v[2] = {rnd64(), rnd64()};
        uint8_t *b = (uint8_t *)v;
        uint32_t m = (uint32_t)(v[0] & 0x7FF);
        if ((m & 0x1FF) == 0x1FC || !astc_decode_block(b, bw, bh, tries & 1, tmp)) continue;
        int p = (int)(v[0] >> 11 & 3), bk = dual_bit(m) ? 4 + p : p;
        if (bk >= NB || have[bk] >= PER_BUCKET) continue;
        have[bk]++, g_bucket[bk]++;
        memcpy(blk + 16 * k++, b, 16);
    }
    for (int i = 0; i < 40 && k < n; i++, g_void++) void_block(blk + 16 * k++, i % 5);
    for (; k < n; k++) {
        uint64_t v[2] = {rnd64(), rnd64()};
        memcpy(blk + 16 * k, v, 16);
        if (!astc_decode_block(blk + 16 * k, bw, bh, false, tmp)) g_err++;
    }
}

typedef struct {
    VkImage img;
    VmaAllocation alloc;
} Img;

static int make_image(VkCtx *c, uint32_t w, uint32_t h, uint32_t levels, int srgb, Img *o) {
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                            .format = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM, .extent = {w, h, 1},
                            .mipLevels = levels, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                     VK_IMAGE_USAGE_SAMPLED_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    return vmaCreateImage(c->vma, &ci, &ai, &o->img, &o->alloc, NULL) == VK_SUCCESS;
}

static void barrier(VkCommandBuffer cb, VkPipelineStageFlags2 src, VkAccessFlags2 sa, VkPipelineStageFlags2 dst,
                    VkAccessFlags2 da) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = src, .srcAccessMask = sa,
                           .dstStageMask = dst, .dstAccessMask = da};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}
#define ALL_RW VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT

static void to_general(VkCommandBuffer cb, VkImage img) {
    VkImageMemoryBarrier2 ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
                                .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .image = img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, 1}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ib};
    vkCmdPipelineBarrier2(cb, &di);
}

static double now_ms(void) { return (double)plat_now_ns() / 1e6; }

// Decodes regions (w0 x h0 at level 0, w1 x h1 at level 1) of blk on the GPU; out_rb: the decoder's texels, out_img:
// the image's two levels read back. 0 on a Vulkan failure.
static int gpu_decode(VkCtx *c, const uint8_t *blk, size_t n0, size_t n1, uint32_t w0, uint32_t h0, uint32_t w1,
                      uint32_t h1, uint32_t bw, uint32_t bh, int srgb, uint8_t *out_rb, uint8_t *out_img, double *ms) {
    Img im;
    if (!make_image(c, w0, h0, 2, srgb, &im)) return 0;
    size_t t0 = (size_t)w0 * h0 * 4, t1 = (size_t)w1 * h1 * 4;
    VkCommandBuffer cb = vk_upload_begin();
    VkBuffer sb, rb, ib;
    VmaAllocation ra, ia;
    uint8_t *stg = vk_staging((n0 + n1) * 16, &sb, NULL);
    uint8_t *rmap = vk_staging(t0 + t1, &rb, &ra), *imap = vk_staging(t0 + t1, &ib, &ia);
    memcpy(stg, blk, (n0 + n1) * 16);
    to_general(cb, im.img);
    VkAstcRegion ar[2] = {{.src = 0, .w = w0, .h = h0, .slices = 1, .level = 0},
                          {.src = n0 * 16, .w = w1, .h = h1, .slices = 1, .level = 1}};
    barrier(cb, ALL_RW, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    int ok = vk_astc_record(cb, sb, (n0 + n1) * 16, ar, 2, bw, bh, srgb, im.img, VK_IMAGE_ASPECT_COLOR_BIT, 0, rb);
    barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL_RW);
    VkBufferImageCopy cp[2] = {{.bufferOffset = 0, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w0, h0, 1}},
                               {.bufferOffset = t0, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 1, 0, 1}, .imageExtent = {w1, h1, 1}}};
    vkCmdCopyImageToBuffer(cb, im.img, VK_IMAGE_LAYOUT_GENERAL, ib, 2, cp);
    barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    vk_upload_end();
    double a = now_ms();
    vk_upload_flush(1);
    if (ms) *ms = now_ms() - a;
    vmaInvalidateAllocation(c->vma, ra, 0, VK_WHOLE_SIZE);
    vmaInvalidateAllocation(c->vma, ia, 0, VK_WHOLE_SIZE);
    if (out_rb) memcpy(out_rb, rmap, t0 + t1);
    if (out_img) memcpy(out_img, imap, t0 + t1);
    vmaDestroyBuffer(c->vma, rb, ra), vmaDestroyBuffer(c->vma, ib, ia);
    vk_defer_image(im.img, im.alloc);
    vk_upload_flush(1);
    return ok;
}

static int first_diff(const uint8_t *a, const uint8_t *b, size_t texels, size_t *at) {
    for (size_t i = 0; i < texels; i++)
        if (memcmp(a + 4 * i, b + 4 * i, 4)) { *at = i; return 1; }
    return 0;
}

static int test_case(VkCtx *c, uint32_t bw, uint32_t bh, int srgb, uint32_t nx, uint32_t ny) {
    // level 0: nx x ny blocks, cropped by 1-3 texels; level 1 half of that, other blocks
    uint32_t w0 = nx * bw - 1 - (bw > 5), h0 = ny * bh - 2, w1 = w0 > 1 ? w0 >> 1 : 1, h1 = h0 > 1 ? h0 >> 1 : 1;
    size_t n0 = (size_t)((w0 + bw - 1) / bw) * ((h0 + bh - 1) / bh), n1 = (size_t)((w1 + bw - 1) / bw) * ((h1 + bh - 1) / bh);
    uint8_t *blk = malloc((n0 + n1) * 16);
    make_blocks(blk, n0, bw, bh);
    make_blocks(blk + n0 * 16, n1, bw, bh);
    size_t t0 = (size_t)w0 * h0, t1 = (size_t)w1 * h1;
    uint8_t *want = malloc((t0 + t1) * 4), *rb = malloc((t0 + t1) * 4), *im = malloc((t0 + t1) * 4);
    memset(rb, 0x5A, (t0 + t1) * 4), memset(im, 0xA5, (t0 + t1) * 4);
    astc_decode_ldr(blk, w0, h0, bw, bh, srgb, want);
    astc_decode_ldr(blk + n0 * 16, w1, h1, bw, bh, srgb, want + t0 * 4);
    int bad = 0;
    if (!gpu_decode(c, blk, n0, n1, w0, h0, w1, h1, bw, bh, srgb, rb, im, NULL)) {
        printf("probe-astc: FAIL %ux%u %s: the GPU decode was not recorded\n", bw, bh, srgb ? "sRGB" : "linear");
        bad = 1;
    }
    for (int lv = 0; lv < 2 && !bad; lv++) {
        size_t off = lv ? t0 * 4 : 0, n = lv ? t1 : t0, at;
        uint32_t w = lv ? w1 : w0;
        for (int which = 0; which < 2 && !bad; which++) {
            const uint8_t *got = (which ? im : rb) + off;
            if (first_diff(want + off, got, n, &at)) {
                const uint8_t *e = want + off + 4 * at, *g = got + 4 * at;
                printf("probe-astc: FAIL %ux%u %s level %d (%s): texel (%zu,%zu) cpu %02x%02x%02x%02x gpu %02x%02x%02x%02x\n",
                       bw, bh, srgb ? "sRGB" : "linear", lv, which ? "image" : "decoder output", at % w, at / w,
                       e[0], e[1], e[2], e[3], g[0], g[1], g[2], g[3]);
                bad = 1;
            }
        }
    }
    g_blocks += n0 + n1, g_texels += t0 + t1;
    free(blk), free(want), free(rb), free(im);
    return bad;
}

// BC7 re-encoding (MKW_ASTC_BC7): constant-colour (void extent) blocks of footprint bw x bh into a BC7 image, read back
// through a blit to RGBA8. A constant colour fits mode 6 within 1 per channel, so a layout or bit-packing mistake shows
// as a large error. Level 0 cropped as in test_case, level 1 half of it.
// kind 2: BC1 instead (opaque ASTC, MKW_ASTC_BC1): RGB only, within the 5.6.5 endpoint rounding.
// kind 3: BC7 of white and black, each transparent and opaque: mode 6 shares one p-bit between the four channels of an
// endpoint, and a transparent texel's alpha must stay exactly 0 (issue #397: transparent white came back with alpha
// 1/255, a faint box around UI images); the rest within 2, as above (opaque black may read alpha 254).
static const uint16_t extremes[4][4] = {{0xFFFF, 0xFFFF, 0xFFFF, 0}, {0, 0, 0, 0xFFFF}, {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF},
                                        {0, 0, 0, 0}};
static int bc7_case(VkCtx *c, uint32_t bw, uint32_t bh, uint32_t nx, uint32_t ny, int kind) {   // nx x ny blocks at level 0
    const char *name = kind == 2 ? "BC1" : kind == 3 ? "BC7 alpha 0" : "BC7";
    uint32_t w0 = nx * bw - 1 - (bw > 5), h0 = ny * bh - 2, w1 = w0 >> 1, h1 = h0 >> 1;
    size_t n0 = (size_t)((w0 + bw - 1) / bw) * ((h0 + bh - 1) / bh), n1 = (size_t)((w1 + bw - 1) / bw) * ((h1 + bh - 1) / bh);
    uint8_t *blk = malloc((n0 + n1) * 16);
    for (size_t k = 0; k < n0 + n1; k++) {
        void_block(blk + 16 * k, 1);
        if (kind == 3) memcpy(blk + 16 * k + 8, extremes[k % 4], 8);   // the void extent's RGBA, UNORM16
    }
    size_t t0 = (size_t)w0 * h0, t1 = (size_t)w1 * h1;
    uint8_t *want = malloc((t0 + t1) * 4);
    astc_decode_ldr(blk, w0, h0, bw, bh, 0, want);
    astc_decode_ldr(blk + n0 * 16, w1, h1, bw, bh, 0, want + t0 * 4);
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                            .format = kind == 2 ? VK_FORMAT_BC1_RGB_UNORM_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK, .extent = {w0, h0, 1}, .mipLevels = 2, .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                     VK_IMAGE_USAGE_SAMPLED_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    Img bc, rgba;
    int bad = 0;
    if (vmaCreateImage(c->vma, &ci, &ai, &bc.img, &bc.alloc, NULL) != VK_SUCCESS || !make_image(c, w0, h0, 2, 0, &rgba)) {
        printf("probe-astc: FAIL %s %ux%u: images\n", name, bw, bh);
        free(blk), free(want);
        return 1;
    }
    VkCommandBuffer cb = vk_upload_begin();
    VkBuffer sb, ib;
    VmaAllocation ia;
    uint8_t *stg = vk_staging((n0 + n1) * 16, &sb, NULL), *imap = vk_staging((t0 + t1) * 4, &ib, &ia);
    memcpy(stg, blk, (n0 + n1) * 16);
    to_general(cb, bc.img), to_general(cb, rgba.img);
    VkAstcRegion ar[2] = {{.src = 0, .w = w0, .h = h0, .slices = 1, .level = 0},
                          {.src = n0 * 16, .w = w1, .h = h1, .slices = 1, .level = 1}};
    barrier(cb, ALL_RW, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    if (!vk_astc_record(cb, sb, (n0 + n1) * 16, ar, 2, bw, bh, 0, bc.img, VK_IMAGE_ASPECT_COLOR_BIT, kind == 3 ? 1 : kind,
                        VK_NULL_HANDLE))
        printf("probe-astc: FAIL %s %ux%u: not recorded\n", name, bw, bh), bad = 1;
    barrier(cb, ALL_RW, ALL_RW);
    for (uint32_t lv = 0; lv < 2; lv++) {
        int32_t w = (int32_t)(lv ? w1 : w0), h = (int32_t)(lv ? h1 : h0);
        VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lv, 0, 1}, .srcOffsets = {{0, 0, 0}, {w, h, 1}},
                          .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lv, 0, 1}, .dstOffsets = {{0, 0, 0}, {w, h, 1}}};
        vkCmdBlitImage(cb, bc.img, VK_IMAGE_LAYOUT_GENERAL, rgba.img, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_NEAREST);
    }
    barrier(cb, ALL_RW, ALL_RW);
    VkBufferImageCopy cp[2] = {{.bufferOffset = 0, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w0, h0, 1}},
                               {.bufferOffset = t0 * 4, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 1, 0, 1}, .imageExtent = {w1, h1, 1}}};
    vkCmdCopyImageToBuffer(cb, rgba.img, VK_IMAGE_LAYOUT_GENERAL, ib, 2, cp);
    barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    vk_upload_end();
    vk_upload_flush(1);
    vmaInvalidateAllocation(c->vma, ia, 0, VK_WHOLE_SIZE);
    int worst = 0;
    size_t at = 0;
    for (size_t i = 0; i < (t0 + t1) * 4 && !bad; i++) {
        if (kind == 2 && i % 4 == 3) continue;   // BC1 RGB: alpha reads 1
        int d = abs((int)imap[i] - (int)want[i]);
        if (kind == 3 && i % 4 == 3 && !want[i] && d) d = 255;   // alpha 0: exact, or it fails
        if (d > worst) worst = d, at = i / 4;
    }
    if (!bad && worst > (kind == 2 ? 5 : 2)) {
        size_t lv = at >= t0, i = lv ? at - t0 : at, w = lv ? w1 : w0;
        const uint8_t *e = want + 4 * at, *g = imap + 4 * at;
        printf("probe-astc: FAIL %s %ux%u level %zu texel (%zu,%zu): want %02x%02x%02x%02x got %02x%02x%02x%02x (error %d)\n",
               name, bw, bh, lv, i % w, i / w, e[0], e[1], e[2], e[3], g[0], g[1], g[2], g[3], worst);
        bad = 1;
    }
    if (!bad) printf("probe-astc: %s %ux%u %ux%u: largest channel error %d\n", name, bw, bh, w0, h0, worst);
    vmaDestroyBuffer(c->vma, ib, ia);
    vk_defer_image(bc.img, bc.alloc), vk_defer_image(rgba.img, rgba.alloc);
    vk_upload_flush(1);
    free(blk), free(want);
    return bad;
}

// One 2048 x 2048 texture of valid blocks: the CPU worker-pool decode against the GPU time of the dispatches alone
// (vk_astc.c timestamps, MKW_ASTC_TIME) and the wall time of the upload batch.
static void timing(VkCtx *c, uint32_t bw, uint32_t bh) {
    uint32_t w = 2048, h = 2048;
    size_t n = (size_t)((w + bw - 1) / bw) * ((h + bh - 1) / bh);
    uint8_t *blk = malloc(n * 16), *out = malloc((size_t)w * h * 4), tmp[12 * 12 * 4];
    for (size_t k = 0; k < 4096;) {   // valid, non-constant blocks: error blocks would flatter the GPU
        uint64_t v[2] = {rnd64(), rnd64()};
        if ((v[0] & 0x1FF) != 0x1FC && astc_decode_block((uint8_t *)v, bw, bh, false, tmp)) memcpy(blk + 16 * k++, v, 16);
    }
    for (size_t k = 4096; k < n; k++) memcpy(blk + 16 * k, blk + 16 * (k % 4096), 16);
    double a = now_ms();
    astc_decode_ldr_mt(blk, w, h, bw, bh, false, out);
    double cpu = now_ms() - a, wall = 0;
    vk_astc_time_enable(1);
    uint64_t t0, c0, t1, c1;
    double u0, u1;
    vk_astc_time_get(bw, bh, &t0, &c0, &u0);
    for (int i = 0; i < 4; i++) gpu_decode(c, blk, n, 1, w, h, 1, 1, bw, bh, 0, NULL, NULL, i ? &wall : NULL);
    vk_astc_time_get(bw, bh, &t1, &c1, &u1);
    vk_astc_time_enable(0);
    double mtex = (double)(t1 - t0) / 1e6;
    printf("probe-astc: 2048x2048 %ux%u: CPU (worker pool) %.1f ms; GPU dispatches %.0f us per M texel (%llu decodes "
           "timed, %.2f M texels), batch wall %.1f ms\n", bw, bh, cpu, mtex > 0 ? (u1 - u0) / mtex : 0.0,
           (unsigned long long)(c1 - c0), mtex, wall);
    free(blk), free(out);
}

int probe_astc(void) {
    if (!vk_init()) { printf("probe-astc: FAIL vk_init\n"); return 1; }
    VkCtx *c = vk_ctx();
    printf("probe-astc: GPU %s, textureCompressionASTC_LDR %d, mode %d\n", c->props.deviceName, c->has_astc_ldr,
           vk_astc_mode());
    int bad = 0;
    if (!vk_astc_gpu()) {
        printf("probe-astc: FAIL the compute decoder is not available (MKW_ASTC=cpu or pipeline creation failed)\n");
        bad = 1;
    }
    for (int i = 0; i < 14 && !bad; i++)
        for (int s = 0; s < 2 && !bad; s++) bad = test_case(c, dims[i][0], dims[i][1], s, 37, 23);
    // over vk_astc.c's 32 MiB scratch chunk at level 0 (45 MiB): cut into bands of whole rows of 12 and 20 texels
    if (!bad) bad = test_case(c, 6, 6, 0, 560, 560) || test_case(c, 10, 10, 1, 340, 340);
    if (!bad && vk_astc_bc7()) {   // MKW_ASTC_BC7=1
        // the 4-aligned footprints; the last one is 4800x2400, over vk_astc.c's 32 MiB scratch chunk at level 0
        bad = bc7_case(c, 4, 4, 37, 23, 1) || bc7_case(c, 8, 8, 37, 23, 1) || bc7_case(c, 12, 12, 37, 23, 1) ||
              bc7_case(c, 8, 8, 600, 300, 1) || bc7_case(c, 8, 8, 37, 23, 2) || bc7_case(c, 8, 8, 600, 300, 2) ||
              bc7_case(c, 8, 8, 37, 23, 3);
    } else if (!bad) {
        printf("probe-astc: BC7 re-encoding off (MKW_ASTC_BC7=1 tests it)\n");
    }
    for (int b = 0; b < NB && !bad; b++)
        if (!g_bucket[b]) { printf("probe-astc: FAIL no valid %s block was generated\n", bucket_name[b]); bad = 1; }
    if (!bad) {
        timing(c, 4, 4);
        timing(c, 6, 6);
        timing(c, 8, 8);
        timing(c, 12, 12);
    }
    vk_mem_shutdown();
    vk_shutdown();
    long errors = vk_validation_errors_total();
    if (bad || errors) {
        printf("probe-astc: FAILED (%s, %ld validation errors)\n", bad ? "a check failed" : "checks passed", errors);
        return 1;
    }
    printf("probe-astc: passed (14 footprints x linear/sRGB, %llu blocks, %llu texels bit-exact with astc_decode_ldr; "
           "valid blocks by kind: 1p %llu, 2p %llu, 3p %llu, 4p %llu, dual 1p %llu, dual 2p %llu, dual 3p %llu; "
           "%llu void extents, %llu random error blocks; 0 validation errors)\n",
           (unsigned long long)g_blocks, (unsigned long long)g_texels, (unsigned long long)g_bucket[0],
           (unsigned long long)g_bucket[1], (unsigned long long)g_bucket[2], (unsigned long long)g_bucket[3],
           (unsigned long long)g_bucket[4], (unsigned long long)g_bucket[5], (unsigned long long)g_bucket[6],
           (unsigned long long)g_void, (unsigned long long)g_err);
    return 0;
}
