// Internals of the recorder thread shared by its files (vk_rec.c: ring, recorder and fixed-size commands;
// vk_rec_cmds.c: commands with arrays and extension commands): packet ops, the packet structs other files decode, and
// the producer's primitives.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "vk.h"

enum {
    R_PAD, R_BeginQuery, R_BeginRendering, R_BindDescriptorSets, R_BindIndexBuffer, R_BindPipeline, R_BindVertexBuffers2,
    R_BlitImage, R_ClearAttachments, R_ClearColorImage, R_ClearDepthStencilImage, R_CopyBuffer, R_CopyBufferToImage,
    R_CopyImage, R_CopyImageToBuffer, R_Dispatch, R_DispatchIndirect, R_Draw, R_DrawIndexed, R_DrawIndexedIndirect, R_DrawIndexedIndirectCount, R_DrawIndirect, R_DrawIndirectCount,
    R_EndQuery, R_EndRendering, R_FillBuffer, R_PipelineBarrier2, R_PushConstants, R_ResetQueryPool, R_ResolveImage,
    R_SetBlendConstants, R_SetCullMode, R_SetDepthBias, R_SetDepthBiasEnable, R_SetDepthCompareOp, R_SetDepthTestEnable,
    R_SetDepthWriteEnable, R_SetFrontFace, R_SetPrimitiveRestartEnable, R_SetPrimitiveTopology,
    R_SetRasterizerDiscardEnable, R_SetScissorWithCount, R_SetStencilCompareMask, R_SetStencilOp, R_SetStencilReference,
    R_SetStencilTestEnable, R_SetStencilWriteMask, R_SetViewportWithCount, R_WriteTimestamp, R_WriteTimestamp2,
    R_PushTpl, R_Push, R_VertexInput, R_Bool, R_PolygonMode, R_DrawMesh, R_BlendEnable, R_BlendEquation, R_WriteMask,
    R_Marker, R_Checkpoint,
};

// Packets with arrays: a struct with the call's arguments, the arrays after it, pointers into the packet.
typedef struct { VkPipelineBindPoint bp; VkPipeline p; } A_BindPipeline;
typedef struct { uint32_t n; const VkViewport *v; } A_SetViewportWithCount;
typedef struct { uint32_t n; const VkRect2D *r; } A_SetScissorWithCount;
typedef struct {
    VkPipelineBindPoint bp;
    VkPipelineLayout l;
    uint32_t first, n, nd;
    const VkDescriptorSet *s;
    const uint32_t *d;
} A_BindDescriptorSets;
typedef struct { uint32_t first, n; const VkBuffer *b; const VkDeviceSize *o, *sz, *st; } A_BindVertexBuffers2;
typedef struct { VkImage s; VkImageLayout sl; VkImage d; VkImageLayout dl; uint32_t n; const void *r; VkFilter f; } A_Img;
typedef struct { uint64_t s, d; VkImageLayout l; uint32_t n; const void *r; } A_Xfer;   // buffer or image handles
typedef struct { uint32_t na, nr; const VkClearAttachment *a; const VkClearRect *r; } A_ClearAttachments;
typedef struct { VkImage i; VkImageLayout l; union { VkClearColorValue c; VkClearDepthStencilValue ds; } v; uint32_t n;
                 const VkImageSubresourceRange *r; } A_ClearImage;
typedef struct { VkPipelineLayout l; VkShaderStageFlags st; uint32_t o, n; const void *v; } A_PushConstants;
typedef struct { PFN_vkCmdPushDescriptorSetWithTemplateKHR fn; VkDescriptorUpdateTemplate tpl; VkPipelineLayout l;
                 uint32_t set; const void *data; } A_PushTpl;
typedef struct { PFN_vkCmdPushDescriptorSetKHR fn; VkPipelineBindPoint bp; VkPipelineLayout l; uint32_t set, n;
                 VkWriteDescriptorSet *w; } A_Push;
typedef struct { PFN_vkCmdSetVertexInputEXT fn; uint32_t nb, na; const VkVertexInputBindingDescription2EXT *b;
                 const VkVertexInputAttributeDescription2EXT *v; } A_VertexInput;
typedef struct { PFN_vkCmdSetDepthClampEnableEXT fn; VkBool32 v; } A_Bool;
typedef struct { PFN_vkCmdSetPolygonModeEXT fn; VkPolygonMode m; } A_PolygonMode;
typedef struct { PFN_vkCmdDrawMeshTasksEXT fn; uint32_t x, y, z; } A_DrawMesh;
typedef struct { PFN_vkCmdWriteBufferMarkerAMD fn; VkPipelineStageFlagBits stage; VkBuffer b; VkDeviceSize o; uint32_t v; } A_Marker;
typedef struct { PFN_vkCmdSetCheckpointNV fn; const void *m; } A_Checkpoint;
typedef struct { void *fn; uint32_t first, n; const void *v; } A_Targets;

extern VkCommandBuffer rec_cb;   // the part being recorded (set while the recorder is idle)
extern int rec_live;             // producer: rec_begin started the recorder for rec_cb
extern uint32_t lz[256];         // MKW_REC_DIFF: words of the packet being built hashed as zero in the loose hash
extern int nlz;
#define OWNED(cb) (rec_live && (cb) == rec_cb)
#define A8(x) (((x) + 7) & ~(size_t)7)
void *rec_put(uint32_t op, size_t bytes);   // room for a packet of bytes of arguments; the caller fills it, then rec_pub
void rec_pub(void);
void rec_fallback(void);                    // a call the ring takes no packet for: everything queued first
// Copies arr after the fixed part at *at and returns where it went (NULL for none).
void *rec_tail_copy(uint8_t **at, const void *arr, size_t bytes);

typedef struct {
    VkCommandBuffer cb;
    uint8_t vp_valid, sc_valid, db_valid, bc_valid, sr_valid[2], vi_valid;
    uint32_t vp_n, sc_n;
    VkViewport vp[16];
    VkRect2D sc[16];
    float db[3];
    float bc[4];
    uint32_t sr[2];
    uint32_t vi_nb, vi_na;
    VkVertexInputBindingDescription2EXT vi_b[17];
    VkVertexInputAttributeDescription2EXT vi_a[16];
} RecDirtyState;

extern _Thread_local RecDirtyState rec_dirty;
int rec_cap_active(void);

static inline RecDirtyState *rec_dirty_for(VkCommandBuffer cb) {
    if (rec_dirty.cb != cb) {
        memset(&rec_dirty, 0, sizeof rec_dirty);
        rec_dirty.cb = cb;
    }
    return &rec_dirty;
}
