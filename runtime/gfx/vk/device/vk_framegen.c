// Frame generation (MKW_FRAMEGEN=off|auto|2|3|4, off by default: the console does not do it): frames made between
// the game's, from its last two, so a game at 30 fps is shown at 60 (2x) and one at 60 at 120 on a fast display.
// MKW hands out no motion vectors, so the motion comes from the pictures (optical flow), on the present thread and the
// GPU only: nothing on the game thread or the replay thread (docs/roadmap.md).
//
// Per game frame (vk_fg_frame, the present thread, before the game frame's timeline value is signalled):
//   1. the crop rectangle of the game's picture is copied into a picture of our own (the game draws over its own next
//      frame) and reduced to a luma pyramid: full size, a quarter, a sixteenth;
//   2. block matching, one vector per 16x16 pixels (32x32 at first): at the sixteenth within +-8 texels (+-128
//      pixels), then at quarter size within +-3 texels around that, then on the picture within +-3 pixels with the
//      fraction of a pixel, each followed by a 3x3 median (shaders/fg_flow.comp, fg_median.comp);
//   3. each made frame at t = k / n (shaders/fg_synth.comp).
// The caller blits every frame (the made ones, then the game's) into a swapchain image of its own at once and then
// presents each at its time, spread evenly over the game's recent frame interval (an average of the pacer's display
// times): the blits never queue behind the game's next frame on the GPU (Hookshot issue #1702: made frames shown 5.7
// and 11 ms apart instead of 8.3). The game's frame is shown (n - 1) / n of a frame later than without: the latency.
// Frames over 100 ms apart (a loading hitch) are not blended. Auto makes as many frames as fit the monitor's refresh
// rate at the game's frame rate (60 Hz at 30 fps: 2x; 144 Hz at 60 fps: 2x; 60 Hz at 60 fps: none).
//
// The algorithm and the display schedule follow SkyboundTotk's rx_framegen.cpp (a64-kit/gpu/render/rx_framegen.cpp,
// https://github.com/NotRexed/SkyboundTotk @ a4495f95b5e5ab6562d603058dcaa3457d0c8027, MIT, Copyright (c) 2026
// NotRexed), ported from WebGPU; the three-level search and the synthesis guards are ours.
#include "vk.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "fg_flow_spv.h"
#include "fg_luma_spv.h"
#include "fg_median_spv.h"
#include "fg_synth_spv.h"
#include "hacks.h"
#include "host.h"

#define SLOTS 4
#define GAP_NS 100000000ull

typedef struct {
    VkImage img;
    VmaAllocation alloc;
    VkImageView view;
} Img;

static struct {
    int tried, ok;
    PFN_vkCmdPushDescriptorSetKHR push;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline luma, coarse, fine, full, median, synth;
    VkSampler sampler;
    VkCommandPool pool;
    VkCommandBuffer cb[SLOTS];
    VkFence fence[SLOTS];
    unsigned n;
    // the images, for a w x h picture: the game's last two (rgba8), their luma at full size (lf), quarter size (lq) and
    // a sixteenth (le), the vectors of each level and their medians (coarse, fine, full size) and the made frames
    Img pic[2], lf[2], lq[2], le[2], fl[6], made[VK_FG_MAX - 1];
    int w, h, fresh, cur, have_prev;
    int nomem_w, nomem_h;   // the size whose images did not fit: not tried again until the size changes
    float lambda, bias;
    // pacing (the present thread)
    uint64_t last_at, interval, frames, made_total;
    int logged_mult, auto_m;   // auto_m: MKW_FRAMEGEN=auto's factor now (mult_now)
} F;

static VkPipeline make_pipeline(VkCtx *c, const uint32_t *code, size_t size, const int *spec, int nspec) {
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = code};
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(c->device, &mi, NULL, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkSpecializationMapEntry me[7];
    for (int i = 0; i < nspec; i++) me[i] = (VkSpecializationMapEntry){(uint32_t)i, (uint32_t)(i * 4), 4};
    VkSpecializationInfo si = {(uint32_t)nspec, me, (size_t)nspec * 4, spec};
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main",
                                                .pSpecializationInfo = nspec ? &si : NULL},
                                      .layout = F.layout};
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
    vkDestroyShaderModule(c->device, m, NULL);
    return p;
}

// MKW_FG_LAMBDA, MKW_FG_BIAS (tuning, with --probe-framegen): the search's cost per pixel of motion and for leaving
// zero, in mean luma difference per patch texel
static float knob(const char *name, float def) {
    const char *e = cfg_get(name);
    return e ? (float)atof(e) : def;
}

static int init(VkCtx *c) {
    F.tried = 1;
    F.push = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(c->device, "vkCmdPushDescriptorSetKHR");
    if (!F.push) {
        rt_log("vk: frame generation off: no VK_KHR_push_descriptor");
        return 0;
    }
    // the luma levels and the vectors are written as storage images in formats Vulkan does not require for that
    static const VkFormat need[] = {VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16G16_SFLOAT};
    for (int i = 0; i < 2; i++) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(c->phys, need[i], &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) {
            rt_log("vk: frame generation off: no storage images of format %d on this GPU", (int)need[i]);
            return 0;
        }
    }
    VkDescriptorSetLayoutBinding b[4];
    for (int i = 0; i < 4; i++)
        b[i] = (VkDescriptorSetLayoutBinding){.binding = (uint32_t)i, .descriptorCount = 1,
                                              .descriptorType = i < 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                                      : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                              .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                           .bindingCount = 4, .pBindings = b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &F.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR,
                              .minFilter = VK_FILTER_LINEAR, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0};
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                   .queueFamilyIndex = c->gfx_family};
    // fg_flow.comp's levels {CELL, HALF, RADIUS, START, STEP, SUBPIX, SPARSE}: one vector per 32x32 pixels from an 8x8
    // patch at a sixteenth (128x128 pixels: a 4x4 one, 64x64, matched blurred text and flat ground anywhere), +-8
    // texels (128 pixels) from zero; 8x8 at quarter size (32x32 pixels), +-3 texels around that; 8x8 samples two pixels
    // apart at full size (the 16x16 block), +-2 pixels around that (+-3 matched more wrongly), with the fraction
    static const int coarse[7] = {2, 4, 8, 0, 1, 0, 0}, fine[7] = {4, 4, 3, 1, 1, 0, 1}, full[7] = {16, 4, 2, 1, 2, 1, 0};
    if (vkCreateDescriptorSetLayout(c->device, &dci, NULL, &F.dsl) != VK_SUCCESS ||
        vkCreatePipelineLayout(c->device, &li, NULL, &F.layout) != VK_SUCCESS ||
        vkCreateSampler(c->device, &si, NULL, &F.sampler) != VK_SUCCESS ||
        vkCreateCommandPool(c->device, &pci, NULL, &F.pool) != VK_SUCCESS ||
        !(F.luma = make_pipeline(c, fg_luma_spv, sizeof fg_luma_spv, NULL, 0)) ||
        !(F.coarse = make_pipeline(c, fg_flow_spv, sizeof fg_flow_spv, coarse, 7)) ||
        !(F.fine = make_pipeline(c, fg_flow_spv, sizeof fg_flow_spv, fine, 7)) ||
        !(F.full = make_pipeline(c, fg_flow_spv, sizeof fg_flow_spv, full, 7)) ||
        !(F.median = make_pipeline(c, fg_median_spv, sizeof fg_median_spv, NULL, 0)) ||
        !(F.synth = make_pipeline(c, fg_synth_spv, sizeof fg_synth_spv, NULL, 0))) {
        rt_log("vk: frame generation off: its pipelines could not be created");
        return 0;
    }
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = F.pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = SLOTS};
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
    if (vkAllocateCommandBuffers(c->device, &ai, F.cb) != VK_SUCCESS) return 0;
    for (int i = 0; i < SLOTS; i++)
        if (vkCreateFence(c->device, &fi, NULL, &F.fence[i]) != VK_SUCCESS) return 0;
    F.lambda = knob("MKW_FG_LAMBDA", 0.0001f);
    F.bias = knob("MKW_FG_BIAS", 0.003f);
    F.interval = 1000000000ull / 60;
    return F.ok = 1;
}

static void free_img(VkCtx *c, Img *m) {
    if (m->view) vkDestroyImageView(c->device, m->view, NULL);
    if (m->img) vmaDestroyImage(c->vma, m->img, m->alloc);
    memset(m, 0, sizeof *m);
}

static void free_images(VkCtx *c) {
    for (int i = 0; i < 2; i++) free_img(c, &F.pic[i]), free_img(c, &F.lf[i]), free_img(c, &F.lq[i]), free_img(c, &F.le[i]);
    for (int i = 0; i < 6; i++) free_img(c, &F.fl[i]);
    for (int i = 0; i < VK_FG_MAX - 1; i++) free_img(c, &F.made[i]);
    F.w = F.h = 0;
    F.have_prev = 0;
}

static int make_img(VkCtx *c, Img *m, uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags extra) {
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | extra};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(c->vma, &ci, &ai, &m->img, &m->alloc, NULL) != VK_SUCCESS) {
        m->img = VK_NULL_HANDLE;
        return 0;
    }
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = m->img,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    return vkCreateImageView(c->device, &vi, NULL, &m->view) == VK_SUCCESS;
}

// The vector grid: one per 16x16 pixels, a texel of the sixteenth level
static uint32_t grid(int n) { return ((uint32_t)n + 15) / 16; }

static int ensure_images(VkCtx *c, int w, int h) {
    if (F.pic[0].img && F.w == w && F.h == h) return 1;
    // A failed size is not tried every present: each try waits for the whole device (below) and frees what it made
    if (F.nomem_w == w && F.nomem_h == h) return 0;
    // A size change: earlier presents may still read the old images (vkDeviceWaitIdle holds the queue lock, as
    // vk_fsr.c ensure_images)
    vk_queue_lock();
    vkDeviceWaitIdle(c->device);
    vk_queue_unlock();
    free_images(c);
    const VkImageUsageFlags XS = VK_IMAGE_USAGE_TRANSFER_SRC_BIT, XD = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    uint32_t qw = ((uint32_t)w + 3) / 4, qh = ((uint32_t)h + 3) / 4, gw = grid(w), gh = grid(h);
    int ok = 1;
    for (int i = 0; i < 2; i++)
        ok = ok && make_img(c, &F.pic[i], (uint32_t)w, (uint32_t)h, VK_FORMAT_R8G8B8A8_UNORM, XS | XD) &&
             make_img(c, &F.lf[i], (uint32_t)w, (uint32_t)h, VK_FORMAT_R16_SFLOAT, 0) &&
             make_img(c, &F.lq[i], qw, qh, VK_FORMAT_R16_SFLOAT, 0) &&
             make_img(c, &F.le[i], gw, gh, VK_FORMAT_R16_SFLOAT, 0);
    for (int i = 0; i < 6; i++)   // (the coarse level's two: one vector per 2x2 of the others)
        ok = ok && make_img(c, &F.fl[i], i < 2 ? (gw + 1) / 2 : gw, i < 2 ? (gh + 1) / 2 : gh, VK_FORMAT_R16G16_SFLOAT, 0);
    for (int i = 0; i < VK_FG_MAX - 1; i++)
        ok = ok && make_img(c, &F.made[i], (uint32_t)w, (uint32_t)h, VK_FORMAT_R8G8B8A8_UNORM, XS);
    if (!ok) {
        rt_log("vk: frame generation: no memory for its %dx%d images; off until the size changes", w, h);
        free_images(c);
        F.nomem_w = w, F.nomem_h = h;
        return 0;
    }
    F.w = w, F.h = h, F.fresh = 1, F.cur = 0;
    rt_log("vk: frame generation: %dx%d pictures, %ux%u vectors", w, h, gw, gh);
    return 1;
}

static void mem_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &d);
}

static void img_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkPipelineStageFlags2 ss, VkAccessFlags2 sa,
                        VkPipelineStageFlags2 ds, VkAccessFlags2 da) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = ss, .srcAccessMask = sa,
                               .dstStageMask = ds, .dstAccessMask = da, .oldLayout = from,
                               .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                          .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &d);
}

// One pass: inputs a, b, c (b and c may repeat a) into out, over w x h invocations, then a barrier
static void pass(VkCommandBuffer cb, VkPipeline p, const Img *a, const Img *b, const Img *c, const Img *out,
                 const void *pc, uint32_t w, uint32_t h) {
    VkDescriptorImageInfo ii[4] = {{F.sampler, a->view, VK_IMAGE_LAYOUT_GENERAL},
                                   {F.sampler, (b ? b : a)->view, VK_IMAGE_LAYOUT_GENERAL},
                                   {F.sampler, (c ? c : a)->view, VK_IMAGE_LAYOUT_GENERAL},
                                   {VK_NULL_HANDLE, out->view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet wr[4];
    for (int i = 0; i < 4; i++)
        wr[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = (uint32_t)i,
                                       .descriptorCount = 1,
                                       .descriptorType = i < 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                               : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                       .pImageInfo = &ii[i]};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    rec_push(F.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, F.layout, 0, 4, wr);
    vkCmdPushConstants(cb, F.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, pc);
    vkCmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
    mem_barrier(cb);
}

int vk_fg_record(VkCommandBuffer cb, VkImage src, int sw, int sh, const int crop[4], int made, VkImage out[VK_FG_MAX],
                 int *ow, int *oh) {
    VkCtx *c = vk_ctx();
    if (!F.tried) init(c);
    if (!F.ok) return 0;
    int cx = 0, cy = 0, cw = sw, ch = sh;
    if (crop && crop[2] > 0 && crop[3] > 0) cx = crop[0], cy = crop[1], cw = crop[2], ch = crop[3];
    if (cx < 0) cx = 0;
    if (cy < 0) cy = 0;
    if (cx + cw > sw) cw = sw - cx;
    if (cy + ch > sh) ch = sh - cy;
    if (cw <= 0 || ch <= 0 || !ensure_images(c, cw, ch)) return 0;
    if (made > VK_FG_MAX - 1) made = VK_FG_MAX - 1;
    if (!F.have_prev) made = 0;
    const int cur = F.cur, prev = cur ^ 1;
    const VkPipelineStageFlags2 ALL = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, XF = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    if (F.fresh) {
        Img *all[] = {&F.pic[0], &F.pic[1], &F.lf[0], &F.lf[1], &F.lq[0], &F.lq[1], &F.le[0],   &F.le[1],   &F.fl[0],
                      &F.fl[1],  &F.fl[2],  &F.fl[3], &F.fl[4], &F.fl[5], &F.made[0], &F.made[1], &F.made[2]};
        for (size_t i = 0; i < sizeof all / sizeof *all; i++)
            img_barrier(cb, all[i]->img, VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_NONE, 0, ALL,
                        VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
        F.fresh = 0;
    }
    // earlier reads of our images (the last frame's presents) finish before this frame writes them; the game's writes
    // to its picture become visible to the copy, and its later writes wait for the copy
    mem_barrier(cb);
    img_barrier(cb, src, VK_IMAGE_LAYOUT_GENERAL, ALL, VK_ACCESS_2_MEMORY_WRITE_BIT, XF, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkImageBlit region = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                          .srcOffsets = {{cx, cy, 0}, {cx + cw, cy + ch, 1}},
                          .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                          .dstOffsets = {{0, 0, 0}, {cw, ch, 1}}};
    vkCmdBlitImage(cb, src, VK_IMAGE_LAYOUT_GENERAL, F.pic[cur].img, VK_IMAGE_LAYOUT_GENERAL, 1, &region, VK_FILTER_NEAREST);
    img_barrier(cb, src, VK_IMAGE_LAYOUT_GENERAL, XF, 0, ALL, 0);
    mem_barrier(cb);
    const uint32_t qw = ((uint32_t)cw + 3) / 4, qh = ((uint32_t)ch + 3) / 4, gw = grid(cw), gh = grid(ch);
    const int colour[4] = {1, 1, 0, 0}, level[4] = {0, 4, 0, 0};
    pass(cb, F.luma, &F.pic[cur], NULL, NULL, &F.lf[cur], colour, (uint32_t)cw, (uint32_t)ch);
    pass(cb, F.luma, &F.lf[cur], NULL, NULL, &F.lq[cur], level, qw, qh);
    pass(cb, F.luma, &F.lq[cur], NULL, NULL, &F.le[cur], level, gw, gh);
    if (made) {
        const float pc16[4] = {16, F.lambda, F.bias, 0}, pc4[4] = {4, F.lambda, F.bias, 0}, pc1[4] = {1, F.lambda, F.bias, 0};
        pass(cb, F.coarse, &F.le[prev], &F.le[cur], NULL, &F.fl[0], pc16, (gw + 1) / 2, (gh + 1) / 2);
        pass(cb, F.median, &F.fl[0], NULL, NULL, &F.fl[1], level, (gw + 1) / 2, (gh + 1) / 2);
        pass(cb, F.fine, &F.lq[prev], &F.lq[cur], &F.fl[1], &F.fl[2], pc4, gw, gh);
        pass(cb, F.median, &F.fl[2], NULL, NULL, &F.fl[3], level, gw, gh);
        pass(cb, F.full, &F.lf[prev], &F.lf[cur], &F.fl[3], &F.fl[4], pc1, gw, gh);
        pass(cb, F.median, &F.fl[4], NULL, NULL, &F.fl[5], level, gw, gh);
        for (int k = 0; k < made; k++) {
            const float pc[4] = {(float)(k + 1) / (float)(made + 1), 1.0f / (16.0f * (float)gw), 1.0f / (16.0f * (float)gh), 0};
            pass(cb, F.synth, &F.pic[prev], &F.pic[cur], &F.fl[5], &F.made[k], pc, (uint32_t)cw, (uint32_t)ch);
        }
    }
    for (int k = 0; k < made; k++) out[k] = F.made[k].img;
    out[made] = F.pic[cur].img;
    *ow = cw, *oh = ch;
    F.have_prev = 1;
    F.cur = prev;
    return made + 1;
}

// The frames per game frame the setting asks for now: 0 off (also under the turbo, whose frames are dropped anyway,
// and while a full-screen movie plays: it goes at the game's own 30 or 60 and a made frame between two of its pictures
// only repeats or smears them; vis3, 15/11/2026: "duplicado cuando no hace falta"), else 1..VK_FG_MAX, at most what
// the swapchain has room for
// Whether m frames per game frame split the screen's refreshes evenly: every frame shown for the same whole number of
// refreshes. The game runs at 60 / iv fps (its present interval, from the measured frame interval); m frames of it
// at hz fit when hz * iv / (60 * m) is a whole number. 20 fps x2 on 60 Hz is 40 fps, frames held 1, 2, 1, 2
// refreshes: judder (Adrià 15/11/2026); 20 x3 and 15 x4 are 60, 15 x2 is 30. hz unknown: taken as fitting.
// The game's interval in 1/60 s: what it asked NVN for (game.c), not a measure. The average in F.interval lags ~8
// frames behind a change (fga2: 41.9 ms at 20 fps), and steps between display times grow when a frame is late: a
// median of the last three read 5 at 15 fps, no factor fit and frame generation paused (fga3, frames 1670-1675).
static int game_ticks(void) {
    int iv = frame_present_interval();
    return iv < 1 ? 1 : iv;
}

static int fits(int m) {
    double hz = vk_present_refresh_hz();
    if (hz <= 0) return 1;
    double k = hz * game_ticks() / (60.0 * m);
    return k >= 0.97 && fabs(k - round(k)) < 0.03 * round(k);
}

static int vrr(void) { return cfg_bool("MKW_VRR"); }

static int mult_now(void) {
    const char *v = cfg_value("MKW_FRAMEGEN");
    if (!strcmp(v, "off") || turbo_speed() > 1 || ov_movie_playing()) return 0;
    int m;
    if (!strcmp(v, "auto") && !vrr()) {
        // the most frames that split the refreshes evenly; none (a 75 Hz screen: no game rate fits) shows the game's
        // own frames. Within what the swapchain has room for.
        int room = vk_present_ahead_max();
        m = 1;
        for (int k = VK_FG_MAX < room ? VK_FG_MAX : room; k >= 2 && m == 1; k--)
            if (fits(k)) m = k;
        return m;
    } else if (!strcmp(v, "auto")) {   // a variable refresh screen shows any rate evenly: as many as fit in time
        // The refreshes of the monitor that fit in one game frame, rounded down: rounded up a quarter early, a game
        // frame of 46 ms at 60 Hz (2.76 refreshes) asked for 3x, made 2 frames where 1.76 fit, so the GPU made frames
        // never shown (fg3-auto: 1.1 made per game frame), and it switched between 2x and 3x every few seconds. Kept
        // within a band: up only 0.1 past the next whole number, down only 0.05 under its own.
        double hz = vk_present_refresh_hz(), r = hz * (double)F.interval / 1e9;
        if (hz <= 0) F.auto_m = 2;
        else if (!F.auto_m || r < F.auto_m - 0.05) F.auto_m = (int)r;
        else if (r >= F.auto_m + 1.1) F.auto_m = (int)(r - 0.1);
        m = F.auto_m;
    } else {
        m = atoi(v);
    }
    int room = vk_present_ahead_max();
    if (m > room) m = room;
    if (m > VK_FG_MAX) m = VK_FG_MAX;
    return m < 1 ? 1 : m;
}

int vk_fg_wanted(void) { return strcmp(cfg_value("MKW_FRAMEGEN"), "off") != 0; }

// The menu's note: a fixed 2x/3x/4x that does not split this screen's refreshes evenly at the game's rate now
int vk_fg_uneven(void) {
    const char *v = cfg_value("MKW_FRAMEGEN");
    if (!strcmp(v, "off") || !strcmp(v, "auto") || vrr() || !F.frames) return 0;
    return !fits(atoi(v));
}

// MKW_FG_DUMP=<game frame> (checking, with MKW_FG_DUMP_DIR, default "."): from that game frame of frame generation on,
// four in a row as g<n>.png and the first frame made before each as m<n>.png, read back on the present thread (it
// waits for the GPU then): tools/imagen/fg_check.py takes three g in a row (the made frame between the first and the
// third against the second). MKW_SHOTS reads the game's pictures, black on some machines; these are what is shown.
static struct { uint64_t at; VkBuffer buf; VmaAllocation alloc; void *map; VkDeviceSize size; } D;

static int dump_record(VkCommandBuffer cb, const VkImage *out, int n, int w, int h) {
    if (!D.at) {
        const char *e = cfg_get("MKW_FG_DUMP");
        D.at = e && atoll(e) > 0 ? (uint64_t)atoll(e) : UINT64_MAX;
    }
    if (F.frames < D.at || F.frames >= D.at + 4 || (D.buf && D.size < (VkDeviceSize)w * h * 8)) return 0;
    VkCtx *c = vk_ctx();
    if (!D.buf) {
        D.size = (VkDeviceSize)w * h * 8;
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)w * h * 8,
                                 .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                      .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
        VmaAllocationInfo info;
        if (vmaCreateBuffer(c->vma, &bi, &ai, &D.buf, &D.alloc, &info) != VK_SUCCESS) return D.at = UINT64_MAX, 0;
        D.map = info.pMappedData;
    }
    VkBufferImageCopy bc = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {(uint32_t)w, (uint32_t)h, 1}};
    vkCmdCopyImageToBuffer(cb, out[n - 1], VK_IMAGE_LAYOUT_GENERAL, D.buf, 1, &bc);
    bc.bufferOffset = (VkDeviceSize)w * h * 4;
    if (n > 1) vkCmdCopyImageToBuffer(cb, out[0], VK_IMAGE_LAYOUT_GENERAL, D.buf, 1, &bc);
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                           .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                           .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &d);
    return 1;
}

static void dump_write(VkFence fence, int n, int w, int h) {
    VkCtx *c = vk_ctx();
    vkWaitForFences(c->device, 1, &fence, VK_TRUE, 5000000000ull);
    vmaInvalidateAllocation(c->vma, D.alloc, 0, VK_WHOLE_SIZE);
    const char *dir = cfg_get("MKW_FG_DUMP_DIR") ? cfg_get("MKW_FG_DUMP_DIR") : ".";
    char path[1024];
    for (int k = 0; k < (n > 1 ? 2 : 1); k++) {
        snprintf(path, sizeof path, "%s/%c%llu.png", dir, k ? 'm' : 'g', (unsigned long long)F.frames);
        rt_write_png(path, (const uint8_t *)D.map + (size_t)k * w * h * 4, (uint32_t)w, (uint32_t)h);
    }
    rt_log("vk: frame generation: game frame %llu written to %s (interval %.1f ms)", (unsigned long long)F.frames, dir,
           F.interval / 1e6);
}

int vk_fg_frame(VkImage src, int sw, int sh, const int crop[4], uint64_t at, VkImage out[VK_FG_MAX], int *w, int *h,
                uint64_t *step) {
    // the game's frame interval: an average of the pacer's display times, a hitch left out
    int gap = !F.last_at || at <= F.last_at || at - F.last_at > GAP_NS;
    if (!gap) {
        uint64_t d = at - F.last_at;
        // no cap under GAP_NS: a 50 ms one held 15 fps (66.7 ms) at 50, so at 2x the made frame and the game's went
        // out 25 and 41.7 ms apart instead of 33 and 33 (vis2: "game frame every 50.0 ms")
        if (d < 4000000) d = 4000000;
        F.interval = F.interval ? (F.interval * 7 + d) / 8 : d;
    }
    F.last_at = at;
    int m = mult_now();
    if (m < 2) {   // off, or auto with no room between two refreshes: the caller presents the game's picture
        if (F.logged_mult >= 2) rt_log("vk: frame generation paused (%s)", ov_movie_playing() ? "a movie plays" : "off");
        F.logged_mult = m;
        F.have_prev = 0;
        return 0;
    }
    VkCtx *c = vk_ctx();
    if (!F.tried) init(c);
    if (!F.ok) return 0;
    if (gap) F.have_prev = 0;
    unsigned slot = F.n++ % SLOTS;
    if (vkWaitForFences(c->device, 1, &F.fence[slot], VK_TRUE, 5000000000ull) != VK_SUCCESS) {
        rt_log("vk: frame generation: fence wait timed out");
        return 0;
    }
    VkCommandBuffer cb = F.cb[slot];
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cb, &bi);
    int n = vk_fg_record(cb, src, sw, sh, crop, m - 1, out, w, h);
    int dump = n && dump_record(cb, out, n, *w, *h);
    vkEndCommandBuffer(cb);
    if (!n) return 0;   // (recorded nothing the GPU must run: the command buffer is reset next time)
    vkResetFences(c->device, 1, &F.fence[slot]);
    VkCommandBufferSubmitInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cb};
    VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = 1, .pCommandBufferInfos = &cbi};
    vk_queue_lock();
    VkResult r = vkQueueSubmit2(c->gfx_queue, 1, &si, F.fence[slot]);
    vk_queue_unlock();
    if (r != VK_SUCCESS) {
        rt_log("vk: frame generation: submit failed (%d)", (int)r);
        F.have_prev = 0;
        return 0;
    }
    if (dump) dump_write(F.fence[slot], n, *w, *h);
    // the step from the game's interval in whole 1/60 s (the average lags ~8 frames behind a change of rate; under the
    // turbo frame generation is off)
    *step = (uint64_t)(game_ticks() * (1e9 / 60.0)) / (uint64_t)n;
    F.frames++, F.made_total += (uint64_t)(n - 1);
    if (m != F.logged_mult || F.frames % 1800 == 0) {   // ~1 min at 30 fps
        F.logged_mult = m;
        rt_log("vk: frame generation %dx: game frame every %.1f ms, %llu frames made over %llu game frames", m,
               F.interval / 1e6, (unsigned long long)F.made_total, (unsigned long long)F.frames);
    }
    return n;
}

VkImage vk_fg_flow(int level, int *gw, int *gh) {
    *gw = (int)grid(F.w), *gh = (int)grid(F.h);
    if (level < 2) *gw = (*gw + 1) / 2, *gh = (*gh + 1) / 2;
    return level >= 0 && level < 6 ? F.fl[level].img : VK_NULL_HANDLE;
}

void vk_fg_release(void) {
    VkCtx *c = vk_ctx();
    if (!c || !F.tried) return;
    if (D.buf) vmaDestroyBuffer(c->vma, D.buf, D.alloc);
    memset(&D, 0, sizeof D);
    free_images(c);
    VkPipeline ps[] = {F.luma, F.coarse, F.fine, F.full, F.median, F.synth};
    for (size_t i = 0; i < sizeof ps / sizeof *ps; i++)
        if (ps[i]) vkDestroyPipeline(c->device, ps[i], NULL);
    for (int i = 0; i < SLOTS; i++)
        if (F.fence[i]) vkDestroyFence(c->device, F.fence[i], NULL);
    if (F.pool) vkDestroyCommandPool(c->device, F.pool, NULL);
    if (F.sampler) vkDestroySampler(c->device, F.sampler, NULL);
    if (F.layout) vkDestroyPipelineLayout(c->device, F.layout, NULL);
    if (F.dsl) vkDestroyDescriptorSetLayout(c->device, F.dsl, NULL);
    memset(&F, 0, sizeof F);
}
