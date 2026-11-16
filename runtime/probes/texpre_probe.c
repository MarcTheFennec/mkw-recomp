// --probe-texpre: quality of the two BC7 encoders of ASTC textures on real game textures. For each "<romfs path>\t<index>"
// line of the file MKW_TEXPRE_PROBE_LIST, level 0 of that BNTX texture is decoded on the CPU (astc_decode_ldr, the
// reference), encoded as BC7 by the GPU encoder (vk_astc_record, mode 6) and by bc7enc.cpp (vk_texpre.c's offline
// encoder), both decoded by the GPU (a blit to RGBA8), and each gets its RGBA PSNR against the reference. Decides
// whether vk_texpre.c (slower than the GPU path) is worth keeping.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

#include "cfg.h"
#include "host.h"
#include "tex.h"
#include "vk.h"
#include "platform/platform.h"

void bc7enc_c_init(void);
void bc7enc_c_block(void *out16, const void *rgba64);

typedef struct { VkImage img; VmaAllocation alloc; } Img;

static int make(VkCtx *c, VkFormat f, uint32_t w, uint32_t h, Img *o) {
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                     VK_IMAGE_USAGE_SAMPLED_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    return vmaCreateImage(c->vma, &ci, &ai, &o->img, &o->alloc, NULL) == VK_SUCCESS;
}

static void barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}

static void to_general(VkCommandBuffer cb, VkImage img) {
    VkImageMemoryBarrier2 ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
                                .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .image = img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ib};
    vkCmdPipelineBarrier2(cb, &di);
}

static double psnr(const uint8_t *a, const uint8_t *b, size_t n) {
    double se = 0;
    for (size_t i = 0; i < n; i++) se += (double)(a[i] - b[i]) * (a[i] - b[i]);
    return se ? 10 * log10(255.0 * 255.0 * (double)n / se) : 99;
}

// The same over premultiplied colour (RGB times alpha, and alpha): what a blend shows, where the colour of a texel of
// alpha 0 counts for nothing and its alpha for everything (issue #397's box came from alpha 1/255 there).
static double psnr_pm(const uint8_t *a, const uint8_t *b, size_t n) {
    double se = 0;
    for (size_t i = 0; i < n; i += 4) {
        for (int ch = 0; ch < 3; ch++) {
            double x = a[i + ch] * a[i + 3] / 255.0, y = b[i + ch] * b[i + 3] / 255.0;
            se += (x - y) * (x - y);
        }
        se += (double)(a[i + 3] - b[i + 3]) * (a[i + 3] - b[i + 3]);
    }
    return se ? 10 * log10(255.0 * 255.0 * (double)n / se) : 99;
}

// Level 0 of texture idx of a BNTX file of the romfs: linear ASTC blocks and the shape; NULL when it is not an ASTC 2D
// texture of one layer.
static uint8_t *bntx_level0(const char *rel, int idx, uint32_t *w, uint32_t *h, uint32_t *bw, uint32_t *bh, int *srgb) {
    static const uint8_t dims[][2] = {{4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6},
                                      {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};
    char path[PLAT_PATH_MAX * 2];
    snprintf(path, sizeof path, "%s/%s", rt_default_romfs, rel);
    uint64_t zn = 0;
    uint8_t *z = plat_file_read(path, &zn), *b = NULL, *lin = NULL;
    unsigned long long n = z ? ZSTD_getFrameContentSize(z, zn) : ZSTD_CONTENTSIZE_ERROR;
    if (n == ZSTD_CONTENTSIZE_ERROR || n == ZSTD_CONTENTSIZE_UNKNOWN || !(b = malloc(n)) ||
        ZSTD_decompress(b, n, z, zn) != n)
        goto out;
    uint64_t infos, o, mipt, data;
    uint32_t fmt, layers, bhl;
    memcpy(&infos, b + 0x28, 8), memcpy(&o, b + infos + 8 * (uint64_t)idx, 8);
    memcpy(&fmt, b + o + 0x1c, 4), memcpy(w, b + o + 0x24, 4), memcpy(h, b + o + 0x28, 4);
    memcpy(&layers, b + o + 0x30, 4), memcpy(&bhl, b + o + 0x34, 4), memcpy(&mipt, b + o + 0x70, 8);
    memcpy(&data, b + mipt, 8);
    if ((fmt >> 8) < 0x2d || (fmt >> 8) > 0x3a || layers != 1 || b[o + 0x11] != 2) goto out;
    *bw = dims[(fmt >> 8) - 0x2d][0], *bh = dims[(fmt >> 8) - 0x2d][1], *srgb = (fmt & 0xff) == 6;
    uint32_t rb = (*w + *bw - 1) / *bw * 16, rows = (*h + *bh - 1) / *bh;
    if ((lin = malloc((size_t)rb * rows))) tex_bl_copy(b + data, lin, rb, rb, rows, 1, bhl, 0, 1);
out:
    free(z), free(b);
    return lin;
}

// One texture: PSNR of the GPU encoder's and of bc7enc's BC7, decoded by the GPU, against the CPU decode.
static int one(VkCtx *c, const char *rel, int idx, double *pg, double *pc) {
    uint32_t w, h, bw, bh;
    int srgb;
    uint8_t *blk = bntx_level0(rel, idx, &w, &h, &bw, &bh, &srgb);
    if (!blk) return 0;
    size_t nb = (size_t)((w + bw - 1) / bw) * ((h + bh - 1) / bh) * 16, nt = (size_t)w * h * 4;
    size_t nc = (size_t)((w + 3) / 4) * ((h + 3) / 4) * 16;
    uint8_t *ref = malloc(nt), *cbc = malloc(nc), *o = cbc;
    astc_decode_ldr(blk, w, h, bw, bh, srgb, ref);
    for (uint32_t by = 0; by < h; by += 4)   // as vk_texpre.c encode_one does it
        for (uint32_t bx = 0; bx < w; bx += 4, o += 16) {
            uint8_t px[64];
            for (uint32_t y = 0; y < 4; y++)
                for (uint32_t x = 0; x < 4; x++) {
                    uint32_t sx = bx + x < w ? bx + x : w - 1, sy = by + y < h ? by + y : h - 1;
                    memcpy(px + 4 * (4 * y + x), ref + 4 * ((size_t)sy * w + sx), 4);
                }
            bc7enc_c_block(o, px);
        }
    Img g, k, rg, rk;
    int ok = make(c, VK_FORMAT_BC7_UNORM_BLOCK, w, h, &g) && make(c, VK_FORMAT_BC7_UNORM_BLOCK, w, h, &k) &&
             make(c, VK_FORMAT_R8G8B8A8_UNORM, w, h, &rg) && make(c, VK_FORMAT_R8G8B8A8_UNORM, w, h, &rk);
    VkBuffer sa, sc, rb;
    VmaAllocation ra;
    VkCommandBuffer cb = vk_upload_begin();
    uint8_t *ast = ok ? vk_staging(nb, &sa, NULL) : NULL, *cst = ok ? vk_staging(nc, &sc, NULL) : NULL;
    uint8_t *back = ok ? vk_staging(2 * nt, &rb, &ra) : NULL;
    if (ast && cst && back) {
        memcpy(ast, blk, nb), memcpy(cst, cbc, nc);
        to_general(cb, g.img), to_general(cb, k.img), to_general(cb, rg.img), to_general(cb, rk.img);
        barrier(cb);
        VkAstcRegion ar = {.src = 0, .w = w, .h = h, .slices = 1};
        ok = vk_astc_record(cb, sa, nb, &ar, 1, bw, bh, srgb, g.img, VK_IMAGE_ASPECT_COLOR_BIT, 1, VK_NULL_HANDLE);
        VkBufferImageCopy cp = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
        vkCmdCopyBufferToImage(cb, sc, k.img, VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
        barrier(cb);
        VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {(int32_t)w, (int32_t)h, 1}},
                          .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {(int32_t)w, (int32_t)h, 1}}};
        vkCmdBlitImage(cb, g.img, VK_IMAGE_LAYOUT_GENERAL, rg.img, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_NEAREST);
        vkCmdBlitImage(cb, k.img, VK_IMAGE_LAYOUT_GENERAL, rk.img, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_NEAREST);
        barrier(cb);
        VkBufferImageCopy c2 = cp;
        c2.bufferOffset = nt;
        vkCmdCopyImageToBuffer(cb, rg.img, VK_IMAGE_LAYOUT_GENERAL, rb, 1, &cp);
        vkCmdCopyImageToBuffer(cb, rk.img, VK_IMAGE_LAYOUT_GENERAL, rb, 1, &c2);
        barrier(cb);
    } else {
        ok = 0;
    }
    vk_upload_end();
    vk_upload_flush(1);
    if (ok) {
        vmaInvalidateAllocation(c->vma, ra, 0, VK_WHOLE_SIZE);
        *pg = psnr(ref, back, nt), *pc = psnr(ref, back + nt, nt);
        printf("probe-texpre: %s %d %ux%u ASTC %ux%u%s: GPU mode 6 %.2f dB, bc7enc %.2f dB; premultiplied GPU %.2f dB, "
               "bc7enc %.2f dB\n", rel, idx, w, h, bw, bh, srgb ? " sRGB" : "", *pg, *pc, psnr_pm(ref, back, nt),
               psnr_pm(ref, back + nt, nt));
    }
    if (back) vmaDestroyBuffer(c->vma, rb, ra);
    vk_defer_image(g.img, g.alloc), vk_defer_image(k.img, k.alloc);
    vk_defer_image(rg.img, rg.alloc), vk_defer_image(rk.img, rk.alloc);
    free(blk), free(ref), free(cbc);
    return ok;
}

static int cmp(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }

int probe_texpre(void) {
    const char *list = cfg_get("MKW_TEXPRE_PROBE_LIST");
    FILE *f = list ? fopen(list, "r") : NULL;
    if (!f) { printf("probe-texpre: FAIL set MKW_TEXPRE_PROBE_LIST to a file of \"<romfs path>\\t<index>\" lines\n"); return 1; }
    if (!vk_init() || !vk_astc_gpu()) { printf("probe-texpre: FAIL vk_init or no GPU ASTC encoder\n"); fclose(f); return 1; }
    bc7enc_c_init();
    VkCtx *c = vk_ctx();
    double g[256], k[256], d[256];
    int n = 0, wins = 0;
    char line[1024], rel[1024];
    int idx;
    while (n < 256 && fgets(line, sizeof line, f))
        if (sscanf(line, "%1023[^\t]\t%d", rel, &idx) == 2 && one(c, rel, idx, &g[n], &k[n])) d[n] = k[n] - g[n], wins += d[n] > 0, n++;
    fclose(f);
    if (!n) { printf("probe-texpre: FAIL no texture\n"); return 1; }
    qsort(g, n, sizeof *g, cmp), qsort(k, n, sizeof *k, cmp), qsort(d, n, sizeof *d, cmp);
    printf("probe-texpre: %d textures; median PSNR GPU mode 6 %.2f dB, bc7enc %.2f dB; bc7enc better in %d; gain median "
           "%.2f dB, min %.2f, max %.2f; worst texture GPU %.2f dB, bc7enc %.2f dB\n",
           n, g[n / 2], k[n / 2], wins, d[n / 2], d[0], d[n - 1], g[0], k[0]);
    return 0;
}
