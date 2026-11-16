// --probe-framegen: frame generation's passes (gfx/vk/device/vk_framegen.c) on two pictures, without the game. Reads
// $MKW_FG_PROBE/a.rgba and b.rgba (uint32 width and height, then the RGBA8 rows; tools/imagen/fg_check.py writes them
// from PNGs), makes the frame halfway between them as the present thread does and writes it to $MKW_FG_PROBE/mid.png,
// with the vectors of each level of the search in $MKW_FG_PROBE/flow.bin (uint32 width and height of the grid, then six
// grids of float16 x, y: vk_fg_flow's levels), then times 20 game frames with one, two and three made frames each (2x,
// 3x, 4x: the GPU's wall time per game frame, its submission included).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "vk.h"
#include "platform/platform.h"

typedef struct { VkImage img; VmaAllocation alloc; } Img;

static uint8_t *read_rgba(const char *dir, const char *name, uint32_t *w, uint32_t *h) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    uint64_t size = 0;
    uint8_t *d = plat_file_read(path, &size);
    if (!d || size < 8) return free(d), NULL;
    memcpy(w, d, 4), memcpy(h, d + 4, 4);
    if (!*w || !*h || *w > 8192 || *h > 8192 || size != 8 + (uint64_t)*w * *h * 4) return free(d), NULL;
    return d;
}

// A GENERAL RGBA8 picture holding px, as the game's would be
static int upload(VkCtx *c, const uint8_t *px, uint32_t w, uint32_t h, Img *o) {
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                     VK_IMAGE_USAGE_SAMPLED_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(c->vma, &ci, &ai, &o->img, &o->alloc, NULL) != VK_SUCCESS) return 0;
    VkBuffer sb;
    VkCommandBuffer cb = vk_upload_begin();
    memcpy(vk_staging((VkDeviceSize)w * h * 4, &sb, NULL), px, (size_t)w * h * 4);
    VkImageMemoryBarrier2 ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
                                .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .image = o->img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ib};
    vkCmdPipelineBarrier2(cb, &di);
    VkBufferImageCopy bc = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
    vkCmdCopyBufferToImage(cb, sb, o->img, VK_IMAGE_LAYOUT_GENERAL, 1, &bc);
    vk_upload_end();
    vk_upload_flush(1);
    return 1;
}

// One game frame through vk_fg_record with `made` frames made; read: the first frame shown copied into rb
static int frame(VkImage src, uint32_t w, uint32_t h, int made, VkBuffer rb, double *ms) {
    VkCommandBuffer cb = vk_upload_begin();
    VkImage out[VK_FG_MAX];
    int ow = 0, oh = 0;
    int n = vk_fg_record(cb, src, (int)w, (int)h, NULL, made, out, &ow, &oh);
    if (n && rb) {
        VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                               .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                               .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};   // (vk_fg_record ends on a barrier: the copy reads its writes)
        VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
        VkBufferImageCopy bc = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
        vkCmdCopyImageToBuffer(cb, out[0], VK_IMAGE_LAYOUT_GENERAL, rb, 1, &bc);
        vkCmdPipelineBarrier2(cb, &di);
    }
    vk_upload_end();
    uint64_t t0 = plat_now_ns();
    vk_upload_flush(1);
    if (ms) *ms = (double)(plat_now_ns() - t0) / 1e6;
    return n;
}

int probe_framegen(void) {
    const char *dir = cfg_get("MKW_FG_PROBE");
    if (!dir) { printf("probe-framegen: FAIL set MKW_FG_PROBE to a folder with a.rgba and b.rgba\n"); return 1; }
    uint32_t w, h, w2, h2;
    uint8_t *a = read_rgba(dir, "a.rgba", &w, &h), *b = read_rgba(dir, "b.rgba", &w2, &h2);
    if (!a || !b || w != w2 || h != h2) { printf("probe-framegen: FAIL no two pictures of the same size in %s\n", dir); return 1; }
    if (!vk_init()) { printf("probe-framegen: FAIL vk_init\n"); return 1; }
    VkCtx *c = vk_ctx();
    Img ia, ib;
    if (!upload(c, a + 8, w, h, &ia) || !upload(c, b + 8, w, h, &ib)) { printf("probe-framegen: FAIL upload\n"); return 1; }
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)w * h * 4,
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VkBuffer rb;
    VmaAllocation ra;
    VmaAllocationInfo info;
    if (vmaCreateBuffer(c->vma, &bi, &ai, &rb, &ra, &info) != VK_SUCCESS) { printf("probe-framegen: FAIL readback\n"); return 1; }
    int ok = frame(ia.img, w, h, 0, VK_NULL_HANDLE, NULL) == 1 && frame(ib.img, w, h, 1, rb, NULL) == 2;
    if (ok) {
        vmaInvalidateAllocation(c->vma, ra, 0, VK_WHOLE_SIZE);
        char path[1024];
        snprintf(path, sizeof path, "%s/mid.png", dir);
        rt_write_png(path, info.pMappedData, w, h);
        // the vectors: into the same buffer (6 grids of 4 bytes a vector are smaller than the picture)
        // (the coarse grids, half as large, in the top left corner of theirs)
        int gw, gh, lw, lh;
        vk_fg_flow(5, &gw, &gh);
        VkCommandBuffer cb = vk_upload_begin();
        for (int l = 0; l < 6; l++) {
            VkImage img = vk_fg_flow(l, &lw, &lh);
            VkBufferImageCopy bc = {.bufferOffset = (VkDeviceSize)l * gw * gh * 4, .bufferRowLength = (uint32_t)gw,
                                    .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                    .imageExtent = {(uint32_t)lw, (uint32_t)lh, 1}};
            vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_GENERAL, rb, 1, &bc);
        }
        VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                               .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                               .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
        VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
        vkCmdPipelineBarrier2(cb, &di);
        vk_upload_end();
        vk_upload_flush(1);
        vmaInvalidateAllocation(c->vma, ra, 0, VK_WHOLE_SIZE);
        snprintf(path, sizeof path, "%s/flow.bin", dir);
        FILE *f = fopen(path, "wb");
        uint32_t dims[2] = {(uint32_t)gw, (uint32_t)gh};
        if (f) fwrite(dims, 4, 2, f), fwrite(info.pMappedData, 4, (size_t)6 * gw * gh, f), fclose(f);
        snprintf(path, sizeof path, "%s/mid.png", dir);
        for (int made = 1; made <= VK_FG_MAX - 1 && ok; made++) {
            double sum = 0, best = 1e9, ms;
            for (int i = 0; i < 20 && ok; i++) {
                ok = frame(i & 1 ? ib.img : ia.img, w, h, made, VK_NULL_HANDLE, &ms) == made + 1;
                sum += ms;
                if (ms < best) best = ms;
            }
            printf("probe-framegen: %ux%u on %s: %dx, %.2f ms a game frame with %d made (mean of 20, best %.2f)\n", w, h,
                   c->props.deviceName, made + 1, sum / 20, made, best);
        }
        printf("probe-framegen: %s\n", path);
    }
    // No device teardown: the process ends here, and vk_shutdown after vk_init stops in VMA's destructor (a pool left
    // over: --probe-astc does the same, 11/11/2026)
    long errors = vk_validation_errors_total();
    if (!ok || errors) {
        printf("probe-framegen: FAILED (%s, %ld validation errors)\n", ok ? "frames made" : "a frame failed", errors);
        return 1;
    }
    printf("probe-framegen: passed\n");
    return 0;
}
