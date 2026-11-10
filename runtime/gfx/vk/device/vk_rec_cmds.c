// The recorder thread's commands with arrays and its extension commands (vk_rec.h): each copies its arguments and arrays
// into one packet of the ring (vk_rec.c) with its pointers into the packet.
#define VK_REC_IMPL
#include <string.h>

#include "vk_rec_diff.h"
#include "vk_rec_int.h"

#define put rec_put
#define pub rec_pub
#define tail_copy rec_tail_copy
#define FALLBACK rec_fallback

// ---- commands with arrays: a struct with the call's arguments, the arrays after it, pointers into the packet

void rc_vkCmdSetViewportWithCount(VkCommandBuffer cb, uint32_t n, const VkViewport *v) {
    RecDirtyState *d = rec_dirty_for(cb);
    if (n <= 16 && v) {
        if (d->vp_valid && d->vp_n == n && !memcmp(d->vp, v, sizeof *v * n) && !rec_cap_active()) return;
        memcpy(d->vp, v, sizeof *v * n);
        d->vp_n = n, d->vp_valid = 1;
    } else {
        d->vp_valid = 0;
    }
    if (!OWNED(cb)) { vkCmdSetViewportWithCount(cb, n, v); return; }
    A_SetViewportWithCount *a = put(R_SetViewportWithCount, sizeof *a + sizeof *v * n);
    uint8_t *at = (uint8_t *)(a + 1);
    a->n = n, a->v = tail_copy(&at, v, sizeof *v * n);
    pub();
}

void rc_vkCmdSetScissorWithCount(VkCommandBuffer cb, uint32_t n, const VkRect2D *r) {
    RecDirtyState *d = rec_dirty_for(cb);
    if (n <= 16 && r) {
        if (d->sc_valid && d->sc_n == n && !memcmp(d->sc, r, sizeof *r * n) && !rec_cap_active()) return;
        memcpy(d->sc, r, sizeof *r * n);
        d->sc_n = n, d->sc_valid = 1;
    } else {
        d->sc_valid = 0;
    }
    if (!OWNED(cb)) { vkCmdSetScissorWithCount(cb, n, r); return; }
    A_SetScissorWithCount *a = put(R_SetScissorWithCount, sizeof *a + sizeof *r * n);
    uint8_t *at = (uint8_t *)(a + 1);
    a->n = n, a->r = tail_copy(&at, r, sizeof *r * n);
    pub();
}

void rc_vkCmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout l, uint32_t first,
                                uint32_t n, const VkDescriptorSet *s, uint32_t nd, const uint32_t *d) {
    if (vk_guard_on()) vk_guard_bound_sets(cb, bp, first, n, s);
    if (!OWNED(cb)) { vkCmdBindDescriptorSets(cb, bp, l, first, n, s, nd, d); return; }
    A_BindDescriptorSets *a = put(R_BindDescriptorSets, sizeof *a + A8(sizeof *s * n) + A8(sizeof *d * nd));
    uint8_t *at = (uint8_t *)(a + 1);
    *a = (A_BindDescriptorSets){bp, l, first, n, nd, NULL, NULL};
    a->s = tail_copy(&at, s, sizeof *s * n), a->d = tail_copy(&at, d, sizeof *d * nd);
    if (rec_diff_on > 0 && nd) {   // the dynamic offsets' words
        uint32_t w0 = (uint32_t)(((uint64_t *)a->d - (uint64_t *)a)), nw = (uint32_t)(A8(sizeof *d * nd) / 8);
        for (uint32_t i = 0; i < nw && nlz < 256; i++) lz[nlz++] = w0 + i;
    }
    pub();
}

void rc_vkCmdBindVertexBuffers2(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer *b, const VkDeviceSize *o,
                                const VkDeviceSize *sz, const VkDeviceSize *st) {
    if (!OWNED(cb)) { vkCmdBindVertexBuffers2(cb, first, n, b, o, sz, st); return; }
    size_t e = sizeof(VkDeviceSize) * n;
    A_BindVertexBuffers2 *a = put(R_BindVertexBuffers2, sizeof *a + A8(sizeof *b * n) + 3 * A8(e));
    uint8_t *at = (uint8_t *)(a + 1);
    a->first = first, a->n = n;
    a->b = tail_copy(&at, b, sizeof *b * n), a->o = tail_copy(&at, o, e);
    a->sz = sz ? tail_copy(&at, sz, e) : NULL, a->st = st ? tail_copy(&at, st, e) : NULL;
    pub();
}

#define IMG_CMD(name, rtype, params, call, ...)                                                                         \
    void rc_vkCmd##name params {                                                                                       \
        if (!OWNED(cb)) { vkCmd##name call; return; }                                                                  \
        A_Img *a = put(R_##name, sizeof *a + sizeof(rtype) * n);                                                       \
        uint8_t *at = (uint8_t *)(a + 1);                                                                              \
        *a = (A_Img){__VA_ARGS__};                                                                                      \
        a->r = tail_copy(&at, r, sizeof(rtype) * n);                                                                   \
        pub();                                                                                                          \
    }
IMG_CMD(BlitImage, VkImageBlit, (VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n,
        const VkImageBlit *r, VkFilter f), (cb, s, sl, d, dl, n, r, f), s, sl, d, dl, n, NULL, f)
IMG_CMD(CopyImage, VkImageCopy, (VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n,
        const VkImageCopy *r), (cb, s, sl, d, dl, n, r), s, sl, d, dl, n, NULL, 0)
IMG_CMD(ResolveImage, VkImageResolve, (VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl,
        uint32_t n, const VkImageResolve *r), (cb, s, sl, d, dl, n, r), s, sl, d, dl, n, NULL, 0)

#define XFER_CMD(name, rtype, params, call, S, D, L)                                                                    \
    void rc_vkCmd##name params {                                                                                       \
        if (!OWNED(cb)) { vkCmd##name call; return; }                                                                  \
        A_Xfer *a = put(R_##name, sizeof *a + sizeof(rtype) * n);                                                      \
        uint8_t *at = (uint8_t *)(a + 1);                                                                              \
        *a = (A_Xfer){(uint64_t)(S), (uint64_t)(D), L, n, NULL};                                                        \
        a->r = tail_copy(&at, r, sizeof(rtype) * n);                                                                   \
        pub();                                                                                                          \
    }
XFER_CMD(CopyBuffer, VkBufferCopy, (VkCommandBuffer cb, VkBuffer s, VkBuffer d, uint32_t n, const VkBufferCopy *r),
         (cb, s, d, n, r), s, d, 0)
XFER_CMD(CopyBufferToImage, VkBufferImageCopy, (VkCommandBuffer cb, VkBuffer s, VkImage d, VkImageLayout dl, uint32_t n,
         const VkBufferImageCopy *r), (cb, s, d, dl, n, r), s, d, dl)
XFER_CMD(CopyImageToBuffer, VkBufferImageCopy, (VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkBuffer d, uint32_t n,
         const VkBufferImageCopy *r), (cb, s, sl, d, n, r), s, d, sl)

void rc_vkCmdClearAttachments(VkCommandBuffer cb, uint32_t na, const VkClearAttachment *ca, uint32_t nr, const VkClearRect *r) {
    if (!OWNED(cb)) { vkCmdClearAttachments(cb, na, ca, nr, r); return; }
    A_ClearAttachments *a = put(R_ClearAttachments, sizeof *a + A8(sizeof *ca * na) + A8(sizeof *r * nr));
    uint8_t *at = (uint8_t *)(a + 1);
    a->na = na, a->nr = nr;
    a->a = tail_copy(&at, ca, sizeof *ca * na), a->r = tail_copy(&at, r, sizeof *r * nr);
    pub();
}

void rc_vkCmdClearColorImage(VkCommandBuffer cb, VkImage i, VkImageLayout l, const VkClearColorValue *c, uint32_t n,
                             const VkImageSubresourceRange *r) {
    if (!OWNED(cb)) { vkCmdClearColorImage(cb, i, l, c, n, r); return; }
    A_ClearImage *a = put(R_ClearColorImage, sizeof *a + sizeof *r * n);
    uint8_t *at = (uint8_t *)(a + 1);
    a->i = i, a->l = l, a->v.c = *c, a->n = n, a->r = tail_copy(&at, r, sizeof *r * n);
    pub();
}
void rc_vkCmdClearDepthStencilImage(VkCommandBuffer cb, VkImage i, VkImageLayout l, const VkClearDepthStencilValue *c,
                                    uint32_t n, const VkImageSubresourceRange *r) {
    if (!OWNED(cb)) { vkCmdClearDepthStencilImage(cb, i, l, c, n, r); return; }
    A_ClearImage *a = put(R_ClearDepthStencilImage, sizeof *a + sizeof *r * n);
    uint8_t *at = (uint8_t *)(a + 1);
    a->i = i, a->l = l, a->v.ds = *c, a->n = n, a->r = tail_copy(&at, r, sizeof *r * n);
    pub();
}

void rc_vkCmdPushConstants(VkCommandBuffer cb, VkPipelineLayout l, VkShaderStageFlags st, uint32_t o, uint32_t n,
                           const void *v) {
    if (!OWNED(cb)) { vkCmdPushConstants(cb, l, st, o, n, v); return; }
    A_PushConstants *a = put(R_PushConstants, sizeof *a + n);
    uint8_t *at = (uint8_t *)(a + 1);
    *a = (A_PushConstants){l, st, o, n, NULL};
    a->v = tail_copy(&at, v, n);
    pub();
}

void rc_vkCmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo *di) {
    if (!OWNED(cb)) { vkCmdPipelineBarrier2(cb, di); return; }
    int chained = di->pNext != NULL;
    for (uint32_t i = 0; i < di->memoryBarrierCount; i++) chained |= di->pMemoryBarriers[i].pNext != NULL;
    for (uint32_t i = 0; i < di->bufferMemoryBarrierCount; i++) chained |= di->pBufferMemoryBarriers[i].pNext != NULL;
    for (uint32_t i = 0; i < di->imageMemoryBarrierCount; i++) chained |= di->pImageMemoryBarriers[i].pNext != NULL;
    if (chained) { FALLBACK(); vkCmdPipelineBarrier2(cb, di); return; }
    size_t m = sizeof *di->pMemoryBarriers * di->memoryBarrierCount;
    size_t b = sizeof *di->pBufferMemoryBarriers * di->bufferMemoryBarrierCount;
    size_t im = sizeof *di->pImageMemoryBarriers * di->imageMemoryBarrierCount;
    VkDependencyInfo *a = put(R_PipelineBarrier2, sizeof *a + A8(m) + A8(b) + A8(im));
    uint8_t *at = (uint8_t *)(a + 1);
    *a = *di;
    a->pMemoryBarriers = tail_copy(&at, di->pMemoryBarriers, m);
    a->pBufferMemoryBarriers = tail_copy(&at, di->pBufferMemoryBarriers, b);
    a->pImageMemoryBarriers = tail_copy(&at, di->pImageMemoryBarriers, im);
    pub();
}

void rc_vkCmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo *ri) {
    if (!OWNED(cb)) { vkCmdBeginRendering(cb, ri); return; }
    int chained = ri->pNext != NULL || (ri->pDepthAttachment && ri->pDepthAttachment->pNext) ||
                  (ri->pStencilAttachment && ri->pStencilAttachment->pNext);
    for (uint32_t i = 0; i < ri->colorAttachmentCount; i++) chained |= ri->pColorAttachments[i].pNext != NULL;
    if (chained) { FALLBACK(); vkCmdBeginRendering(cb, ri); return; }
    size_t c = sizeof *ri->pColorAttachments * ri->colorAttachmentCount, one = sizeof(VkRenderingAttachmentInfo);
    VkRenderingInfo *a = put(R_BeginRendering, sizeof *a + A8(c) + 2 * A8(one));
    uint8_t *at = (uint8_t *)(a + 1);
    *a = *ri;
    a->pColorAttachments = tail_copy(&at, ri->pColorAttachments, c);
    a->pDepthAttachment = ri->pDepthAttachment ? tail_copy(&at, ri->pDepthAttachment, one) : NULL;
    a->pStencilAttachment = ri->pStencilAttachment ? tail_copy(&at, ri->pStencilAttachment, one) : NULL;
    pub();
}

// ---- extension commands through function pointers

void rec_push_tpl(PFN_vkCmdPushDescriptorSetWithTemplateKHR fn, VkCommandBuffer cb, VkDescriptorUpdateTemplate tpl,
                  VkPipelineLayout layout, uint32_t set, const void *data, uint32_t bytes) {
    if (!OWNED(cb)) { fn(cb, tpl, layout, set, data); return; }
    A_PushTpl *a = put(R_PushTpl, sizeof *a + bytes);
    uint8_t *at = (uint8_t *)(a + 1);
    *a = (A_PushTpl){fn, tpl, layout, set, NULL};
    a->data = tail_copy(&at, data, bytes);
    if (rec_diff_on > 0) {   // buffer descriptors (24-byte slots whose third word is a range, not a layout): buffer, offset
        const uint64_t *sw = a->data;
        uint32_t w0 = (uint32_t)((const uint64_t *)a->data - (uint64_t *)a);
        for (uint32_t i = 0; i + 3 <= bytes / 8 && nlz < 255; i += 3)
            if (sw[i + 2] >= 16 && sw[i + 2] <= 65536) lz[nlz++] = w0 + i, lz[nlz++] = w0 + i + 1;
    }
    pub();
}

void rec_push(PFN_vkCmdPushDescriptorSetKHR fn, VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout,
              uint32_t set, uint32_t n, const VkWriteDescriptorSet *w) {
    if (vk_guard_on()) vk_guard_pushed(cb, bp, set);
    if (!OWNED(cb)) { fn(cb, bp, layout, set, n, w); return; }
    size_t bytes = A8(sizeof *w * n);
    int chained = 0;
    for (uint32_t i = 0; i < n; i++) {
        chained |= w[i].pNext != NULL;
        if (w[i].pBufferInfo) bytes += A8(sizeof *w[i].pBufferInfo * w[i].descriptorCount);
        if (w[i].pImageInfo) bytes += A8(sizeof *w[i].pImageInfo * w[i].descriptorCount);
        if (w[i].pTexelBufferView) bytes += A8(sizeof *w[i].pTexelBufferView * w[i].descriptorCount);
    }
    if (chained) { FALLBACK(); fn(cb, bp, layout, set, n, w); return; }
    A_Push *a = put(R_Push, sizeof *a + bytes);
    uint8_t *at = (uint8_t *)(a + 1);
    *a = (A_Push){fn, bp, layout, set, n, NULL};
    a->w = tail_copy(&at, w, sizeof *w * n);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t k = w[i].descriptorCount;
        a->w[i].pBufferInfo = tail_copy(&at, w[i].pBufferInfo, w[i].pBufferInfo ? sizeof *w[i].pBufferInfo * k : 0);
        a->w[i].pImageInfo = tail_copy(&at, w[i].pImageInfo, w[i].pImageInfo ? sizeof *w[i].pImageInfo * k : 0);
        a->w[i].pTexelBufferView =
            tail_copy(&at, w[i].pTexelBufferView, w[i].pTexelBufferView ? sizeof *w[i].pTexelBufferView * k : 0);
    }
    pub();
}

void rec_vertex_input(PFN_vkCmdSetVertexInputEXT fn, VkCommandBuffer cb, uint32_t nb,
                      const VkVertexInputBindingDescription2EXT *b, uint32_t na,
                      const VkVertexInputAttributeDescription2EXT *v) {
    RecDirtyState *d = rec_dirty_for(cb);
    if (nb <= 17 && na <= 16) {
        if (d->vi_valid && d->vi_nb == nb && d->vi_na == na &&
            (!nb || !memcmp(d->vi_b, b, sizeof *b * nb)) &&
            (!na || !memcmp(d->vi_a, v, sizeof *v * na)) && !rec_cap_active())
            return;
        if (nb) memcpy(d->vi_b, b, sizeof *b * nb);
        if (na) memcpy(d->vi_a, v, sizeof *v * na);
        d->vi_nb = nb, d->vi_na = na, d->vi_valid = 1;
    } else {
        d->vi_valid = 0;
    }
    if (!OWNED(cb)) { fn(cb, nb, b, na, v); return; }
    A_VertexInput *a = put(R_VertexInput, sizeof *a + A8(sizeof *b * nb) + A8(sizeof *v * na));
    uint8_t *at = (uint8_t *)(a + 1);
    *a = (A_VertexInput){fn, nb, na, NULL, NULL};
    a->b = tail_copy(&at, b, sizeof *b * nb), a->v = tail_copy(&at, v, sizeof *v * na);   // pNext: always NULL there
    pub();
}

void rec_bool(PFN_vkCmdSetDepthClampEnableEXT fn, VkCommandBuffer cb, VkBool32 v) {
    if (!OWNED(cb)) { fn(cb, v); return; }
    A_Bool *a = put(R_Bool, sizeof *a);
    *a = (A_Bool){fn, v};
    pub();
}

void rec_polygon_mode(PFN_vkCmdSetPolygonModeEXT fn, VkCommandBuffer cb, VkPolygonMode m) {
    if (!OWNED(cb)) { fn(cb, m); return; }
    A_PolygonMode *a = put(R_PolygonMode, sizeof *a);
    *a = (A_PolygonMode){fn, m};
    pub();
}

void rec_draw_mesh(PFN_vkCmdDrawMeshTasksEXT fn, VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z) {
    if (!OWNED(cb)) { fn(cb, x, y, z); return; }
    A_DrawMesh *a = put(R_DrawMesh, sizeof *a);
    *a = (A_DrawMesh){fn, x, y, z};
    pub();
}

void rec_marker(PFN_vkCmdWriteBufferMarkerAMD fn, VkCommandBuffer cb, VkPipelineStageFlagBits stage, VkBuffer b,
                VkDeviceSize o, uint32_t v) {
    if (!OWNED(cb)) { fn(cb, stage, b, o, v); return; }
    A_Marker *a = put(R_Marker, sizeof *a);
    *a = (A_Marker){fn, stage, b, o, v};
    pub();
}

void rec_checkpoint(PFN_vkCmdSetCheckpointNV fn, VkCommandBuffer cb, const void *m) {
    if (!OWNED(cb)) { fn(cb, m); return; }
    A_Checkpoint *a = put(R_Checkpoint, sizeof *a);
    *a = (A_Checkpoint){fn, m};
    pub();
}

#define TARGET_CMD(fname, R, PFN, T)                                                                                    \
    void fname(PFN fn, VkCommandBuffer cb, uint32_t first, uint32_t n, const T *v) {                                    \
        if (!OWNED(cb)) { fn(cb, first, n, v); return; }                                                               \
        A_Targets *a = put(R, sizeof *a + sizeof *v * n);                                                              \
        uint8_t *at = (uint8_t *)(a + 1);                                                                              \
        *a = (A_Targets){(void *)fn, first, n, NULL};                                                                   \
        a->v = tail_copy(&at, v, sizeof *v * n);                                                                       \
        pub();                                                                                                          \
    }
TARGET_CMD(rec_blend_enable, R_BlendEnable, PFN_vkCmdSetColorBlendEnableEXT, VkBool32)
TARGET_CMD(rec_blend_equation, R_BlendEquation, PFN_vkCmdSetColorBlendEquationEXT, VkColorBlendEquationEXT)
TARGET_CMD(rec_write_mask, R_WriteMask, PFN_vkCmdSetColorWriteMaskEXT, VkColorComponentFlags)
