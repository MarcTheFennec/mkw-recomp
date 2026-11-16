// Graphics and compute pipelines of the Vulkan replay (descriptor layouts: vk_pipe_layout.c). The op-stream replay (vk_exec.c)
// collects NVN state into a VkPipeState and asks for the pipeline of each draw; this file maps that state to Vulkan,
// numbers the descriptor bindings and keeps the caches.
//
// Binding model. The translator (tools/xlate/Program.cs, Ryujinx 1.3.2's SPIR-V back end) decorates every resource of a
// shader with DescriptorSet 0 and numbers the bindings of each shader from 1 in the order it meets them (Accessor
// Create*Binding); binding 0 is Ryujinx's support buffer (ResourceManager: SupportBuffer.Binding, a std140 uniform
// buffer every shader declares). The numbering is per shader, so the vertex and fragment stages of one program collide.
// The scheme here:
//   - one descriptor set, set 0, per pipeline; no push constants (Ryujinx's SPIR-V uses none);
//   - binding 0: the support buffer, one uniform buffer shared by all stages of the pipeline;
//   - vertex stage: its own bindings 1..n unchanged; fragment stage: its bindings b >= 1 move to b + n (the SPIR-V
//     Binding decorations are rewritten when the module is created); compute: its own bindings;
//   - descriptor types from the reflection: constant buffers UNIFORM_BUFFER, storage buffers STORAGE_BUFFER, textures
//     COMBINED_IMAGE_SAMPLER (UNIFORM_TEXEL_BUFFER for buffer textures, SAMPLED_IMAGE for separate ones), images
//     STORAGE_IMAGE (STORAGE_TEXEL_BUFFER for buffer images), array lengths as descriptor counts;
//   - set 0 is a push-descriptor set (VK_KHR_push_descriptor) when its descriptors fit maxPushDescriptors (32 on the RX
//     9070; a pipeline layout may hold one push set only, which rules out a set per stage); larger ones (a vertex and a
//     fragment stage near the census maxima of 15 and 27) are ordinary sets the replay allocates (VkPipeInfo.push 0).
// A fixed layout per stage combination (Ryujinx's own back end: a binding per constant buffer, storage buffer, texture
// and image slot per stage) would hold about 150 descriptors and could never be pushed, so layouts are made per distinct
// binding list and shared by every pipeline with that list; VkPipeInfo.bindings tells the replay which NVN slot feeds
// each binding.
//
// Fragment inputs the vertex stage does not write (stages are translated separately; MNG nvn_mtl.m msl_varyings) get
// outputs added to the vertex module, as Vulkan rejects such pairs, initialised to (0, 0, 0, 1) as Maxwell reads an
// unwritten output (docs/maxwell.md 1.2). The unwritten components of an output the vertex stage does write
// start at (0, 0, 0, 1) in the translator (tools/xlate/patches/0006).
//
// Pipeline key. Everything Vulkan 1.3 can take as dynamic state is dynamic: viewports and scissors with count, depth
// bias, blend constants, stencil masks and references, cull mode, front face, topology (within its class), depth and
// stencil tests and ops, depth bias enable, rasterizer discard; vertex input (VK_EXT_vertex_input_dynamic_state, else
// binding strides only); and, where VK_EXT_extended_dynamic_state3 offers them, depth clamp, polygon mode, blend enables,
// equations, write masks and alpha to coverage. The key holds the rest: shaders, target formats, sample count, topology
// class, depth clip mode, logic op and what is not dynamic on the GPU. The VkPipelineCache persists to
// pipeline-cache.bin next to mkw (MKW_PIPELINE_CACHE overrides the path).
//
// Provenance: the NVN -> Vulkan enum mappings (blend factors and equations, compare functions, stencil ops), the vertex
// format table with its 3-component fallback and the pipeline cache file handling follow LAS Recomp
// runtime/nvn/vulkan/exec.c and device.c @ da35b83a (MIT, Copyright (c) 2026 Peiikai); the pipeline state the key holds
// (program, target formats, blend and write masks, vertex layout, sample count) and the varying fix-up follow
// MacNativeGames games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m pso_key_fill / pso_desc / msl_varyings and its vfmt
// table @ b75be8cf (MIT, Copyright (c) 2026 jhaizhou); the binding conventions are those of Ryujinx's shader translator
// (MIT, tools/vendor/ryujinx).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "solid_spv.h"
#include "vk_pipe_internal.h"
#include "vk_spirv_patch.h"
#include "gfx.h"
#include "host.h"
#include "nvn_hmap.h"
#include "platform/platform.h"
#include "tex.h"

#define LOG_ONCE(...) do { static volatile int32_t once_; if (!plat_xchg32(&once_, 1)) rt_log(__VA_ARGS__); } while (0)

// ---- device capabilities and the pipeline cache (set up on first use, under P.lock)

PipeGlobals P = {.lock = PLAT_MUTEX_INIT};

static Pipe *pipes[PIPE_BUCKETS];

uint64_t pipe_fnv(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

// Hash of a pipeline table key, 8 bytes per step: a draw hashes its whole GKey, and byte-wise FNV over it cost more
// than the lookup. Only the in-memory table uses it; the key file keeps fnv.
uint64_t key_hash(const void *k, size_t n) { return nvn_hm_hash(k, (uint32_t)n); }

// The cache file lives in the selected game folder, so every installation (the main build and each private fork) has its own:
// forks that shared build\v121's cache broke each other's runs (2026-10-22).
static void cache_path_init(void) {
    const char *cfg_path = cfg_get("MKW_PIPELINE_CACHE");
    if (cfg_path && *cfg_path && strlen(cfg_path) < sizeof P.cache_path) {
        strcpy(P.cache_path, cfg_path);
        return;
    }
    P.cache_path[0] = 0;
    snprintf(P.cache_path, sizeof P.cache_path, "%s/pipeline-cache.bin", cfg_game_dir());
}

// The cache's own leftovers: a run killed while saving it leaves a full-size temporary file (plat_file_write_atomic).
static void cache_tmp_cleanup(void) {
    int n = P.cache_path[0] ? plat_file_atomic_cleanup(P.cache_path) : 0;
    if (n) rt_log("vk_pipeline: deleted %d unfinished cache writes of ended runs", n);
}

// Cache data whose header names another device or driver is dropped before Vulkan sees it.
static void cache_load(VkCtx *c) {
    cache_path_init();
    cache_tmp_cleanup();
    void *data = NULL;
    size_t size = 0;
    FILE *f = P.cache_path[0] ? fopen(P.cache_path, "rb") : NULL;
    if (f) {
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (len > (long)sizeof(VkPipelineCacheHeaderVersionOne) && (data = malloc((size_t)len)) &&
            fread(data, 1, (size_t)len, f) == (size_t)len)
            size = (size_t)len;
        fclose(f);
        VkPipelineCacheHeaderVersionOne h;
        if (size) memcpy(&h, data, sizeof h);
        if (size && (h.headerVersion != VK_PIPELINE_CACHE_HEADER_VERSION_ONE || h.vendorID != c->props.vendorID ||
                     h.deviceID != c->props.deviceID || memcmp(h.pipelineCacheUUID, c->props.pipelineCacheUUID, VK_UUID_SIZE))) {
            rt_log("vk_pipeline: %s was written by another device or driver; starting empty", P.cache_path);
            size = 0;
        }
    }
    VkPipelineCacheCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, .initialDataSize = size,
                                    .pInitialData = size ? data : NULL};
    if (vkCreatePipelineCache(c->device, &ci, NULL, &P.cache) != VK_SUCCESS) {
        ci.initialDataSize = 0, ci.pInitialData = NULL;
        if (vkCreatePipelineCache(c->device, &ci, NULL, &P.cache) != VK_SUCCESS) P.cache = VK_NULL_HANDLE;
    }
    free(data);
}

int ensure_init(void) {
    if (P.ready) return 1;
    if (!vk_init()) return 0;
    plat_mutex_lock(&P.lock);
    if (P.ready) {
        plat_mutex_unlock(&P.lock);
        return 1;
    }
    VkCtx *c = vk_ctx();
    P.device = c->device;
    VkPhysicalDevicePushDescriptorPropertiesKHR pd = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &pd};
    vkGetPhysicalDeviceProperties2(c->phys, &p2);
    P.max_push = pd.maxPushDescriptors;
    // MKW_DRAW_CACHE=1: descriptor sets are kept across frames by their contents (vk_exec_dcache.c). A frame binds
    // the same descriptors as the last one but for its uniform blocks' addresses (recdiff.py), so uniform buffers are
    // dynamic and their offsets go with the bind; sets are allocated, as push sets cannot be kept.
    P.draw_cache = cfg_get("MKW_DRAW_CACHE") && atoi(cfg_get("MKW_DRAW_CACHE"));
    P.max_dyn_ubo = p2.properties.limits.maxDescriptorSetUniformBuffersDynamic;
    // MKW_VK_SHARED_LIMIT=<bytes>: less workgroup memory than the device has, to check spirv_clamp_shared's cut and
    // wrap on a desktop GPU as on Adreno 7xx's 32 KiB
    uint32_t sdev = p2.properties.limits.maxComputeSharedMemorySize;
    const char *sl = cfg_get("MKW_VK_SHARED_LIMIT");
    P.smem_limit = sl && *sl && (uint32_t)atoi(sl) && (uint32_t)atoi(sl) < sdev ? (uint32_t)atoi(sl) : sdev;
    if (P.smem_limit != sdev)
        rt_log("vk_pipeline: workgroup memory limited to %u bytes (MKW_VK_SHARED_LIMIT; the device has %u)", P.smem_limit,
               sdev);
    if (P.draw_cache)
        rt_log("vk_pipeline: draw cache on (MKW_DRAW_CACHE): sets kept across frames, up to %u dynamic uniform buffers "
               "a layout", P.max_dyn_ubo);
    P.indep = vk_caps.gpl;
    P.gpl = vk_caps.gpl;
    rt_log(P.gpl ? "vk_pipeline: graphics pipeline library on: a draw whose pipeline is building gets a fast-linked one, "
                   "the optimised build replaces it when done"
                 : "vk_pipeline: graphics pipeline library off: draws wait for or skip pipelines that are building");
    const char *ps = cfg_get("MKW_PUSH_STAGE"), *vo = cfg_get("MKW_VS_OUTS");
    P.push_stage = ps && !strcmp(ps, "fs") ? GFX_STAGE_FS : GFX_STAGE_VS;
    P.vs_outs_all = vo && !strcmp(vo, "all");
    if (P.indep) rt_log("vk_pipeline: stage-independent layouts: vertex set 0, fragment set 1, %s set pushed; vertex outputs: %s",
                        P.push_stage == GFX_STAGE_VS ? "vertex" : "fragment", P.vs_outs_all ? "all 32 locations" : "union of paired fragment inputs");
    // vk_device.c enables every feature the GPU reports in this chain, so the query tells what is enabled.
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT e3 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
    VkPhysicalDeviceProvokingVertexFeaturesEXT pvf = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT,
                                                      .pNext = c->has_eds3 ? &e3 : NULL};
    VkPhysicalDeviceVulkan13Features v13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                            .pNext = c->has_provoking_vertex ? (void *)&pvf : (c->has_eds3 ? (void *)&e3 : NULL)};
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &v13};
    vkGetPhysicalDeviceFeatures2(c->phys, &f2);
    P.pv_last = c->has_provoking_vertex && pvf.provokingVertexLast;
    if (!P.pv_last) rt_log("vk_pipeline: provokingVertexLast unavailable: flat outputs come from the first vertex (NVN: last)");
    // NVIDIA warps have 32 lanes. Pin 32 where possible; the translator partitions wider subgroups when it cannot.
    if (v13.subgroupSizeControl) {
        VkPhysicalDeviceVulkan13Properties p13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES};
        VkPhysicalDeviceProperties2 q2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &p13};
        vkGetPhysicalDeviceProperties2(c->phys, &q2);
        P.sg32_stages = vk_caps.sg_pin_stages;   // vk_caps.c: 32 unless MKW_VK_SUBGROUP; 0 without the pin
        rt_log("vk_pipeline: subgroup size %u-%u, requiredSubgroupSizeStages %#x, pinning %u on %#x", p13.minSubgroupSize,
               p13.maxSubgroupSize, (unsigned)p13.requiredSubgroupSizeStages, vk_caps.sg_size, (unsigned)P.sg32_stages);
    }
    P.f_logic_op = f2.features.logicOp;
    P.f_depth_clamp = f2.features.depthClamp;
    P.f_fill_non_solid = f2.features.fillModeNonSolid;
    P.f_dual_src = f2.features.dualSrcBlend;
#define PROC(field, name) (P.field = (void *)vkGetDeviceProcAddr(c->device, name))
    PROC(push, "vkCmdPushDescriptorSetKHR");
    // Descriptor update templates (core 1.1; the push variant comes with VK_KHR_push_descriptor on 1.1): the driver
    // walks a layout prepared when the layout is made instead of parsing write structs every draw (DXVK does so for the
    // same reason). MKW_DESC_TEMPLATE=0 keeps the write structs.
    if (!cfg_bool("MKW_DESC_TEMPLATE")) {
        rt_log("vk_pipeline: descriptor update templates off (MKW_DESC_TEMPLATE=0)");
    } else if (c->props.apiVersion >= VK_API_VERSION_1_1 && PROC(tpl_create, "vkCreateDescriptorUpdateTemplate") &&
               PROC(tpl_destroy, "vkDestroyDescriptorUpdateTemplate") && PROC(tpl_update, "vkUpdateDescriptorSetWithTemplate")) {
        P.tpl = 1;
        if (!PROC(tpl_push, "vkCmdPushDescriptorSetWithTemplateKHR"))
            rt_log("vk_pipeline: vkCmdPushDescriptorSetWithTemplateKHR missing: push sets are written with write structs");
    } else {
        rt_log("vk_pipeline: descriptor update templates unavailable: sets are written with write structs");
    }
    // MKW_VK_DYN_VERTEX=0/1 forces dynamic vertex input off or on. Default off on Intel's frozen Windows driver for its
    // older integrated GPUs (UHD 630, Ice Lake: Vulkan 1.3.215 and older), which draws nothing with it; LAS Recomp
    // turns it off there for the same reason.
    const char *dv = cfg_get("MKW_VK_DYN_VERTEX");   // no fixed default: it depends on the GPU, see above
    int intel_legacy = c->props.vendorID == 0x8086 && c->props.apiVersion <= VK_MAKE_API_VERSION(0, 1, 3, 215);
    int dv_on = dv && *dv ? strcmp(dv, "0") != 0 : !intel_legacy;
    P.dyn_vi = dv_on && c->has_vertex_input_dynamic_state && PROC(set_vi, "vkCmdSetVertexInputEXT");
    if (!dv_on && c->has_vertex_input_dynamic_state)
        rt_log("vk_pipeline: dynamic vertex input off (%s)", dv && *dv ? "MKW_VK_DYN_VERTEX=0" : "Intel's older Windows driver draws nothing with it");
    if (c->has_eds3) {
        P.dyn_clamp = e3.extendedDynamicState3DepthClampEnable && P.f_depth_clamp && PROC(set_clamp, "vkCmdSetDepthClampEnableEXT");
        P.dyn_poly = e3.extendedDynamicState3PolygonMode && PROC(set_poly, "vkCmdSetPolygonModeEXT");
        P.dyn_blend_en = e3.extendedDynamicState3ColorBlendEnable && PROC(set_blend_en, "vkCmdSetColorBlendEnableEXT");
        P.dyn_blend_eq = e3.extendedDynamicState3ColorBlendEquation && PROC(set_blend_eq, "vkCmdSetColorBlendEquationEXT");
        P.dyn_wmask = e3.extendedDynamicState3ColorWriteMask && PROC(set_wmask, "vkCmdSetColorWriteMaskEXT");
        P.dyn_a2c = e3.extendedDynamicState3AlphaToCoverageEnable && PROC(set_a2c, "vkCmdSetAlphaToCoverageEnableEXT");
    }
#undef PROC
    static const VkDynamicState core[] = {
        VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT, VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT, VK_DYNAMIC_STATE_DEPTH_BIAS,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE,
        VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP,
        VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE,
        VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE};
    P.n_dyn = 0;
    for (size_t i = 0; i < sizeof core / sizeof *core; i++) P.dyn[P.n_dyn++] = core[i];
    P.dyn[P.n_dyn++] = P.dyn_vi ? VK_DYNAMIC_STATE_VERTEX_INPUT_EXT : VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE;
    if (P.dyn_clamp) P.dyn[P.n_dyn++] = VK_DYNAMIC_STATE_DEPTH_CLAMP_ENABLE_EXT;
    if (P.dyn_poly) P.dyn[P.n_dyn++] = VK_DYNAMIC_STATE_POLYGON_MODE_EXT;
    if (P.dyn_blend_en) P.dyn[P.n_dyn++] = VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT;
    if (P.dyn_blend_eq) P.dyn[P.n_dyn++] = VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT;
    if (P.dyn_wmask) P.dyn[P.n_dyn++] = VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT;
    if (P.dyn_a2c) P.dyn[P.n_dyn++] = VK_DYNAMIC_STATE_ALPHA_TO_COVERAGE_ENABLE_EXT;
    cache_load(c);
    rt_log("vk_pipeline: max push descriptors %u; dynamic vertex input %d, depth clamp %d, polygon mode %d, blend enable %d, "
           "blend equation %d, write mask %d, alpha to coverage %d; cache %s",
           P.max_push, P.dyn_vi, P.dyn_clamp, P.dyn_poly, P.dyn_blend_en, P.dyn_blend_eq, P.dyn_wmask, P.dyn_a2c,
           P.cache_path[0] ? P.cache_path : "<memory only>");
    keys_open();
    plat_xchg32(&P.ready, 1);
    plat_mutex_unlock(&P.lock);
    prebuild_start();
    return 1;
}

// ---- NVN -> Vulkan mappings (LAS Recomp exec.c blend_factor, blend_op, compare_fn, stencil_op)

static VkBlendFactor blend_factor(unsigned f) {
    switch (f) {
    case 1: return VK_BLEND_FACTOR_ZERO;
    case 2: return VK_BLEND_FACTOR_ONE;
    case 3: return VK_BLEND_FACTOR_SRC_COLOR;
    case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_DST_ALPHA;
    case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 9: return VK_BLEND_FACTOR_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x10: return VK_BLEND_FACTOR_SRC1_COLOR;
    case 0x11: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case 0x12: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case 0x13: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    case 0x61: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x62: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x63: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x64: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: LOG_ONCE("vk_pipeline: unknown NVN blend factor %#x, using ONE", f); return VK_BLEND_FACTOR_ONE;
    }
}

static VkBlendOp blend_op(unsigned e) {
    switch (e) {
    case 2: return VK_BLEND_OP_SUBTRACT;
    case 3: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 4: return VK_BLEND_OP_MIN;
    case 5: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}

// NVN_DEPTH_FUNC_* / NVN_STENCIL_FUNC_* are NEVER = 1 .. ALWAYS = 8 in VkCompareOp order.
static VkCompareOp compare_op(unsigned f) { return f >= 1 && f <= 8 ? (VkCompareOp)(f - 1) : VK_COMPARE_OP_ALWAYS; }
// NVN_STENCIL_OP_* are KEEP = 1 .. DECR_WRAP = 8 in VkStencilOp order.
static VkStencilOp stencil_op(unsigned o) { return o >= 1 && o <= 8 ? (VkStencilOp)(o - 1) : VK_STENCIL_OP_KEEP; }
// NVN_POLYGON_MODE_POINT/LINE/FILL = 0/1/2, VkPolygonMode FILL/LINE/POINT = 0/1/2.
static VkPolygonMode polygon_mode(unsigned m) {
    if (!P.f_fill_non_solid || m > 2) return VK_POLYGON_MODE_FILL;
    return (VkPolygonMode)(2 - m);
}

VkPrimitiveTopology vk_pipeline_topology(int prim) {
    switch (prim) {
    case NVN_DRAW_PRIMITIVE_POINTS: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case NVN_DRAW_PRIMITIVE_LINES: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case NVN_DRAW_PRIMITIVE_LINE_LOOP:
    case NVN_DRAW_PRIMITIVE_LINE_STRIP: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP:
    case NVN_DRAW_PRIMITIVE_QUAD_STRIP: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;   // same triangles, same coverage
    case NVN_DRAW_PRIMITIVE_TRIANGLE_FAN:
    case NVN_DRAW_PRIMITIVE_POLYGON: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    case NVN_DRAW_PRIMITIVE_LINES_ADJACENCY: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
    case NVN_DRAW_PRIMITIVE_LINE_STRIP_ADJACENCY: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY;
    case NVN_DRAW_PRIMITIVE_TRIANGLES_ADJACENCY: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY;
    case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP_ADJACENCY: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY;
    case NVN_DRAW_PRIMITIVE_PATCHES: return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
    default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;   // TRIANGLES; QUADS after the replay's index rewrite
    }
}

// Topology class (a pipeline's static topology only fixes the class): 0 point, 1 line, 2 triangle, 3 patch.
static int topology_class(VkPrimitiveTopology t) {
    switch (t) {
    case VK_PRIMITIVE_TOPOLOGY_POINT_LIST: return 0;
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
    case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY:
    case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY: return 1;
    case VK_PRIMITIVE_TOPOLOGY_PATCH_LIST: return 3;
    default: return 2;
    }
}

// Optimal-tiling (o = 1) or buffer (o = 0) features of a core format, cached.
static VkFormatFeatureFlags format_features(VkFormat f, int optimal) {
    static VkFormatFeatureFlags known[2][256];
    static uint8_t have[2][256];
    VkCtx *c = vk_ctx();
    if (!c) return 0;
    // MKW_REPLAY_PAR's chunk threads look formats up at once: known is written before have is published
    if ((unsigned)f < 256 && __atomic_load_n(&have[optimal][f], __ATOMIC_ACQUIRE)) return known[optimal][f];
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(c->phys, f, &p);
    VkFormatFeatureFlags r = optimal ? p.optimalTilingFeatures : p.bufferFeatures;
    if ((unsigned)f < 256) known[optimal][f] = r, __atomic_store_n(&have[optimal][f], 1, __ATOMIC_RELEASE);
    return r;
}

// Vertex attribute formats by NVNformat (LAS Recomp exec.c vertex_format; MNG nvn_mtl.m vfmt for the Switch 1 additions).
typedef struct { VkFormat vk; uint8_t type; } VtxFmt;
#define F(v) {VK_FORMAT_##v, GFX_TYPE_FLOAT}
#define U(v) {VK_FORMAT_##v, GFX_TYPE_UINT}
#define I(v) {VK_FORMAT_##v, GFX_TYPE_SINT}
static const VtxFmt vtx_formats[0x9B] = {
    [0x01] = F(R8_UNORM), [0x02] = F(R8_SNORM), [0x03] = U(R8_UINT), [0x04] = I(R8_SINT), [0x05] = F(R16_SFLOAT),
    [0x06] = F(R16_UNORM), [0x07] = F(R16_SNORM), [0x08] = U(R16_UINT), [0x09] = I(R16_SINT), [0x0A] = F(R32_SFLOAT),
    [0x0B] = U(R32_UINT), [0x0C] = I(R32_SINT), [0x0D] = F(R8G8_UNORM), [0x0E] = F(R8G8_SNORM), [0x0F] = U(R8G8_UINT),
    [0x10] = I(R8G8_SINT), [0x11] = F(R16G16_SFLOAT), [0x12] = F(R16G16_UNORM), [0x13] = F(R16G16_SNORM),
    [0x14] = U(R16G16_UINT), [0x15] = I(R16G16_SINT), [0x16] = F(R32G32_SFLOAT), [0x17] = U(R32G32_UINT),
    [0x18] = I(R32G32_SINT), [0x19] = F(R8G8B8_UNORM), [0x1A] = F(R8G8B8_SNORM), [0x1B] = U(R8G8B8_UINT),
    [0x1C] = I(R8G8B8_SINT), [0x1D] = F(R16G16B16_SFLOAT), [0x1E] = F(R16G16B16_UNORM), [0x1F] = F(R16G16B16_SNORM),
    [0x20] = U(R16G16B16_UINT), [0x21] = I(R16G16B16_SINT), [0x22] = F(R32G32B32_SFLOAT), [0x23] = U(R32G32B32_UINT),
    [0x24] = I(R32G32B32_SINT), [0x25] = F(R8G8B8A8_UNORM), [0x26] = F(R8G8B8A8_SNORM), [0x27] = U(R8G8B8A8_UINT),
    [0x28] = I(R8G8B8A8_SINT), [0x29] = F(R16G16B16A16_SFLOAT), [0x2A] = F(R16G16B16A16_UNORM),
    [0x2B] = F(R16G16B16A16_SNORM), [0x2C] = U(R16G16B16A16_UINT), [0x2D] = I(R16G16B16A16_SINT),
    [0x2E] = F(R32G32B32A32_SFLOAT), [0x2F] = U(R32G32B32A32_UINT), [0x30] = I(R32G32B32A32_SINT),
    [0x3D] = F(A2B10G10R10_UNORM_PACK32), [0x3E] = U(A2B10G10R10_UINT_PACK32), [0x3F] = F(B10G11R11_UFLOAT_PACK32),
    // *_UI2F / *_I2F: integer data read as float; the 8- and 16-bit ones are Vulkan's SCALED formats
    [0x51] = F(R8_USCALED), [0x52] = F(R8_SSCALED), [0x53] = F(R16_USCALED), [0x54] = F(R16_SSCALED),
    [0x57] = F(R8G8_USCALED), [0x58] = F(R8G8_SSCALED), [0x59] = F(R16G16_USCALED), [0x5A] = F(R16G16_SSCALED),
    [0x5D] = F(R8G8B8_USCALED), [0x5E] = F(R8G8B8_SSCALED), [0x5F] = F(R16G16B16_USCALED), [0x60] = F(R16G16B16_SSCALED),
    [0x63] = F(R8G8B8A8_USCALED), [0x64] = F(R8G8B8A8_SSCALED), [0x65] = F(R16G16B16A16_USCALED),
    [0x66] = F(R16G16B16A16_SSCALED),
    // 32-bit ones have no SCALED format: read as integers (values are not converted; logged when used)
    [0x55] = U(R32_UINT), [0x56] = I(R32_SINT), [0x5B] = U(R32G32_UINT), [0x5C] = I(R32G32_SINT),
    [0x61] = U(R32G32B32_UINT), [0x62] = I(R32G32B32_SINT), [0x67] = U(R32G32B32A32_UINT), [0x68] = I(R32G32B32A32_SINT),
    [0x69] = F(A2B10G10R10_SNORM_PACK32), [0x6A] = I(A2B10G10R10_SINT_PACK32), [0x6B] = F(A2B10G10R10_USCALED_PACK32),
    [0x6C] = F(A2B10G10R10_SSCALED_PACK32), [0x9A] = F(B8G8R8A8_UNORM),
};
#undef F
#undef U
#undef I

// 3-component 8/16-bit formats are not vertex formats on every GPU: read 4 components (LAS vertex_format_supported).
static VkFormat widen(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_R8G8B8_SNORM: return VK_FORMAT_R8G8B8A8_SNORM;
    case VK_FORMAT_R8G8B8_UINT: return VK_FORMAT_R8G8B8A8_UINT;
    case VK_FORMAT_R8G8B8_SINT: return VK_FORMAT_R8G8B8A8_SINT;
    case VK_FORMAT_R8G8B8_USCALED: return VK_FORMAT_R8G8B8A8_USCALED;
    case VK_FORMAT_R8G8B8_SSCALED: return VK_FORMAT_R8G8B8A8_SSCALED;
    case VK_FORMAT_R16G16B16_SFLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case VK_FORMAT_R16G16B16_UNORM: return VK_FORMAT_R16G16B16A16_UNORM;
    case VK_FORMAT_R16G16B16_SNORM: return VK_FORMAT_R16G16B16A16_SNORM;
    case VK_FORMAT_R16G16B16_UINT: return VK_FORMAT_R16G16B16A16_UINT;
    case VK_FORMAT_R16G16B16_SINT: return VK_FORMAT_R16G16B16A16_SINT;
    case VK_FORMAT_R16G16B16_USCALED: return VK_FORMAT_R16G16B16A16_USCALED;
    case VK_FORMAT_R16G16B16_SSCALED: return VK_FORMAT_R16G16B16A16_SSCALED;
    default: return VK_FORMAT_UNDEFINED;
    }
}

VkFormat vk_vertex_format(int nvn_format, int *type) {
    if (type) *type = GFX_TYPE_FLOAT;
    if (nvn_format <= 0 || nvn_format >= (int)(sizeof vtx_formats / sizeof *vtx_formats)) return VK_FORMAT_UNDEFINED;
    const VtxFmt *v = &vtx_formats[nvn_format];
    if (v->vk == VK_FORMAT_UNDEFINED) return VK_FORMAT_UNDEFINED;
    if (type) *type = v->type;
    if ((nvn_format >= 0x55 && nvn_format <= 0x68) && v->type != GFX_TYPE_FLOAT)
        LOG_ONCE("vk_pipeline: 32-bit *_UI2F/*_I2F vertex formats are read as integers (first: %#x)", nvn_format);
    VkFormat f = v->vk;
    if (vk_ctx() && !(format_features(f, 0) & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)) {
        VkFormat w = widen(f);
        if (w == VK_FORMAT_UNDEFINED || !(format_features(w, 0) & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)) {
            LOG_ONCE("vk_pipeline: vertex format %d (NVN %#x) unsupported", (int)f, nvn_format);
            return VK_FORMAT_UNDEFINED;
        }
        LOG_ONCE("vk_pipeline: vertex format %d unsupported, read as 4 components (first: NVN %#x)", (int)f, nvn_format);
        f = w;
    }
    return f;
}

// Attachment format of an NVN format (tex_vk_format: the format the images are created with).
static VkFormat target_format(int nvn_format, int *depth, int *stencil) {
    TexVkFormat t;
    if (depth) *depth = 0;
    if (stencil) *stencil = 0;
    if (!nvn_format || !tex_vk_format(nvn_format, &t)) return VK_FORMAT_UNDEFINED;
    if (depth) *depth = t.depth;
    if (stencil) *stencil = t.stencil;
    return t.vk;
}

void vk_pipe_state_defaults(VkPipeState *s) {
    memset(s, 0, sizeof *s);
    s->samples = 1;
    s->prim = NVN_DRAW_PRIMITIVE_TRIANGLES;
    for (int i = 0; i < 8; i++)
        s->blend[i] = (blend_st){(uint64_t)i, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_FUNC_ONE,
                                 NVN_BLEND_FUNC_ZERO, NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD};
    s->color = (color_st){0, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS};
    s->chmask.mask = 0xffffffffu;
    s->ms.enable = 1;
    s->poly = (poly_st){NVN_FACE_NONE, NVN_FRONT_FACE_CCW, NVN_POLYGON_MODE_FILL, 0};
    s->ds = (ds_st){0, 1, NVN_DEPTH_FUNC_LESS, 0, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_FUNC_ALWAYS,
                    NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP,
                    NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP};
}

// The colour targets' types: the fragment shader writes raw bits through float outputs, as SM86 does, and a float
// output into a UINT/SINT target is undefined in Vulkan (the race's mesh draws into R8_UINT, issue #64 on Intel Arc).
// MKW_INT_OUTPUTS=0 keeps the float outputs (to compare).
void vk_pipeline_answers(const VkPipeState *s, int stage, GfxShaderAnswers *a) {
    static int outs = -1;
    if (outs < 0) outs = !cfg_get("MKW_INT_OUTPUTS") || strcmp(cfg_get("MKW_INT_OUTPUTS"), "0") != 0;
    if (stage == GFX_STAGE_FS) {
        for (int i = 0; outs && i < 8; i++)
            if (s->color_fmt[i]) a->out_type[i] = (uint8_t)vk_format_numeric(target_format(s->color_fmt[i], NULL, NULL));
        return;
    }
    for (int i = 0; i < 16; i++) {
        va_st va;
        memcpy(&va, &s->vattr[i], sizeof va);
        int type = GFX_TYPE_FLOAT;
        if (va.fmt) vk_vertex_format((int)va.fmt, &type);
        a->attr_type[i] = (uint8_t)type;
    }
}

// ---- vertex input (pipeline state, or vkCmdSetVertexInputEXT)

// An attribute the game left without a format is a fixed one on Maxwell, a single 32-bit component of 0 (deko3d
// gpu_3d_vbo.cpp binds unused attributes as IsFixed 1x32 float): it reads (0, 0, 0, 1), as Vulkan expands a
// one-component format. TotK's foliage shaders scale wind sway by 1 - colour.w; shapes without a vertex colour
// (Brightbloom stem and leaves) must not sway, or they bend away from the parts that have one (the bulb).
static VkFormat zero_format(int type) {
    return type == GFX_TYPE_UINT ? VK_FORMAT_R32_UINT : type == GFX_TYPE_SINT ? VK_FORMAT_R32_SINT : VK_FORMAT_R32_SFLOAT;
}

// Attributes for the Locations the vertex shader reads; one without a usable NVN attribute reads VK_PIPE_ZERO_BINDING.
void vertex_input(const VkPipeState *s, VtxInput *v) {
    GfxShaderIo io[32];
    uint32_t used = gfx_shader_inputs(s->vs, io), streams = 0;
    int zero = 0;
    memset(v, 0, sizeof *v);
    if (gfx_shader_is_mesh_cs_vs(s->vs)) {   // task/mesh as compute: the vertices its mesh pass wrote, one buffer
        for (uint32_t l = 0; l < MESH_CS_VWORDS / 4; l++)
            v->a[v->n_attr++] = (VkVertexInputAttributeDescription2EXT){
                .sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT, .location = l, .binding = 0,
                .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 16 * l};
        v->b[v->n_bind++] = (VkVertexInputBindingDescription2EXT){
            .sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT, .binding = 0, .stride = MESH_CS_VWORDS * 4,
            .inputRate = VK_VERTEX_INPUT_RATE_VERTEX, .divisor = 1};
        return;
    }
    for (int l = 0; l < 32; l++) {
        if (!(used >> l & 1)) continue;
        VkVertexInputAttributeDescription2EXT *a = &v->a[v->n_attr++];
        *a = (VkVertexInputAttributeDescription2EXT){.sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT,
                                                     .location = (uint32_t)l};
        va_st va = {0};
        if (l < 16) memcpy(&va, &s->vattr[l], sizeof va);
        VkFormat f = va.fmt ? vk_vertex_format((int)va.fmt, NULL) : VK_FORMAT_UNDEFINED;
        if (f != VK_FORMAT_UNDEFINED && va.stream < 16) {
            a->binding = va.stream, a->format = f, a->offset = va.offset;
            streams |= 1u << va.stream;
        } else {
            a->binding = VK_PIPE_ZERO_BINDING, a->format = zero_format(io[l].type);
            zero = 1;
        }
    }
    for (uint32_t b = 0; b < 16; b++) {
        if (!(streams >> b & 1)) continue;
        uint32_t div = s->vstream[b].divisor;
        if (div > 1) LOG_ONCE("vk_pipeline: instance divisor %u treated as 1 (no VK_EXT_vertex_attribute_divisor)", div);
        v->b[v->n_bind++] = (VkVertexInputBindingDescription2EXT){
            .sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT, .binding = b, .stride = s->vstream[b].stride,
            .inputRate = div ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX, .divisor = 1};
    }
    if (zero)
        v->b[v->n_bind++] = (VkVertexInputBindingDescription2EXT){
            .sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT, .binding = VK_PIPE_ZERO_BINDING,
            .stride = 0, .inputRate = VK_VERTEX_INPUT_RATE_VERTEX, .divisor = 1};
}

int vk_pipeline_vertex_input_dynamic(void) { return ensure_init() && P.dyn_vi; }

PFN_vkCmdPushDescriptorSetKHR vk_pipeline_push_fn(void) { return ensure_init() ? P.push : NULL; }
PFN_vkCmdPushDescriptorSetWithTemplateKHR vk_pipeline_push_tpl_fn(void) { return ensure_init() && P.tpl ? P.tpl_push : NULL; }
PFN_vkUpdateDescriptorSetWithTemplate vk_pipeline_update_tpl_fn(void) { return ensure_init() && P.tpl ? P.tpl_update : NULL; }
int vk_pipeline_draw_cache(void) { return ensure_init() && P.draw_cache; }

// ---- SPIR-V rewrites: vk_spirv_patch.c

// Chain {size 32} into a stage create info when the stage uses subgroup ops and the driver allows the pin.
void pin_subgroup32(VkPipelineShaderStageCreateInfo *st, VkPipelineShaderStageRequiredSubgroupSizeCreateInfo *rq,
                           const uint32_t *w, uint64_t words) {
    if (!(P.sg32_stages & st->stage) || !w || !uses_subgroup_ops(w, words)) return;
    *rq = (VkPipelineShaderStageRequiredSubgroupSizeCreateInfo){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO, .requiredSubgroupSize = vk_caps.sg_size};
    st->pNext = rq;
}

VkShaderModule make_module(const uint32_t *w, uint64_t words) {
    uint64_t fixed_n = 0;
    uint32_t *fixed = spirv_fixup(w, words, &fixed_n);
    if (fixed) w = fixed, words = fixed_n;
    uint32_t from = 0, to = 0, smax = P.smem_limit;
    uint32_t *clamped = spirv_clamp_shared(w, words, smax, &fixed_n, &from, &to);
    if (clamped) {
        if (from != to)
            LOG_ONCE("vk_pipeline: compute shaders declare up to %u bytes of shared memory, the limit is %u: cut to %u",
                     from, smax, to);
        free(fixed);
        fixed = clamped, w = fixed, words = fixed_n;
    }
    VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = (size_t)words * 4, .pCode = w};
    VkShaderModule m = VK_NULL_HANDLE;
    VkResult r = vkCreateShaderModule(P.device, &ci, NULL, &m);
    free(fixed);
    return r == VK_SUCCESS ? m : VK_NULL_HANDLE;
}

// ---- pipeline cache table

Pipe *pipe_find(const void *key, size_t size, uint64_t h) {
    for (Pipe *p = pipes[h % PIPE_BUCKETS]; p; p = p->next)
        if (p->hash == h && p->key_size == size && !memcmp(p->key, key, size)) return p;
    return NULL;
}

// Stores a result (a failed one too, so it is not retried every draw). Caller holds P.lock.
Pipe *pipe_add(const void *key, size_t size, uint64_t h, VkPipeline pipeline, const Layout *l, const Bindings *B,
                      VkPipelineBindPoint bp, uint32_t vs_inputs) {
    Pipe *p = calloc(1, sizeof *p + size);
    if (!p) return NULL;
    p->hash = h, p->key_size = size;
    memcpy(p->key, key, size);
    if (l && B) {
        p->bindings = malloc(B->n * sizeof *p->bindings);
        if (p->bindings) memcpy(p->bindings, B->pb, B->n * sizeof *p->bindings);
    }
    p->info = (VkPipeInfo){pipeline, l ? l->layout : VK_NULL_HANDLE, l ? l->set : VK_NULL_HANDLE, l ? l->push : 0, bp,
                           p->bindings ? B->n : 0, p->bindings, vs_inputs};
    p->info.set_layout1 = l ? l->set1 : VK_NULL_HANDLE;
    p->info.push1 = l ? l->push1 : 0;
    p->info.n_sets = l && l->set1 ? 2 : 1;
    if (l) p->info.tpl[0] = l->tpl[0], p->info.tpl[1] = l->tpl[1];
    p->lay = l;
    Pipe **head = &pipes[h % PIPE_BUCKETS];
    p->next = *head;
    *head = p;
    return p;
}

// ---- graphics pipelines

// A task/mesh draw's state: vs holds the task program (mesh in VkPipeState.mesh) or the mesh program alone.
int mesh_pipe(const VkPipeState *s) { return s->vs && gfx_shader_mesh_kind(s->vs) != GFX_MESH_NONE; }

static int blendable(VkFormat f) {
    return f != VK_FORMAT_UNDEFINED && (format_features(f, 1) & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT);
}

static int target_count(const VkPipeState *s) {
    int n = 0;
    for (int i = 0; i < 8; i++)
        if (s->color_fmt[i]) n = i + 1;
    return n;
}

// SRC1 blend factors read the fragment shader's second output (dual-source blending); the pipeline then needs a
// fragment module whose output 1 is Location 0, Index 1 (spirv_dual_source).
static int is_src1(unsigned f) { return f >= NVN_BLEND_FUNC_SRC1_COLOR && f <= NVN_BLEND_FUNC_ONE_MINUS_SRC1_ALPHA; }
// Runs per draw (gkey_fill, target_wmask): only the blend factors of blend-enabled targets are read until one is SRC1,
// which TotK never sets (0 of 876 pipelines in the field and fight saves).
static int uses_src1(const VkPipeState *s) {
    for (uint32_t m = s->color.blend_en & 0xffu; m; m &= m - 1) {
        int i = __builtin_ctz(m);
        const blend_st *b = &s->blend[i];
        if ((is_src1(b->src_rgb) || is_src1(b->dst_rgb) || is_src1(b->src_a) || is_src1(b->dst_a)) &&
            i < target_count(s) && blendable(target_format(s->color_fmt[i], NULL, NULL)))
            return 1;
    }
    return 0;
}

// Colour write mask of target i: NVN's channel mask, zero for a target outside the fragment shader's output map (SPH
// omap). Maxwell leaves such a target untouched (nxnative 04-graphics.md, "Targets the fragment shader does not
// write"); Vulkan writes undefined values, and on AMD they lit grass in front of Link as flat lime. Zeros are wrong
// too: TotK's half-res transparency pass binds its linear depth (R32F) as target 7, samples it in the same draws and
// writes only targets 0-2, 4 and 5; zeroed, the depth that later passes rebuild from it was 0 in tiles, and the fog
// over distant geometry showed rectangles of clear sky.
static uint32_t target_wmask(const VkPipeState *s, int i) {
    uint32_t m = s->chmask.mask >> (4 * i) & 0xf;
    if (s->fs && !(gfx_shader_outputs(s->fs, NULL) >> i & 1)) m = 0;
    if (i == 1 && s->fs && P.f_dual_src && uses_src1(s)) m = 0;   // output 1 is the second blend source (spirv_dual_source)
    return m;
}

uint32_t vk_pipeline_target_wmask(const VkPipeState *s, int i) { return target_wmask(s, i); }

void gkey_fill(const VkPipeState *s, GKey *k) {
    memset(k, 0, sizeof *k);
    k->vs = s->vs, k->fs = s->fs;
    k->indep = (uint8_t)P.indep;
    if (mesh_pipe(s)) k->meshp = 1, k->mesh = s->mesh, k->indep = 0;
    k->n_color = (uint8_t)target_count(s);
    for (int i = 0; i < k->n_color; i++) k->color[i] = target_format(s->color_fmt[i], NULL, NULL);
    int d, st;
    VkFormat df = target_format(s->depth_fmt, &d, &st);
    k->depth = d ? df : VK_FORMAT_UNDEFINED;
    k->stencil = st ? df : VK_FORMAT_UNDEFINED;
    k->samples = s->samples > 1 ? s->samples : 1;
    k->topo_class = k->meshp ? 2 : (uint8_t)topology_class(vk_pipeline_topology(s->prim));
    k->minus_w = s->depth_minus_w != 0;
    k->dsb = s->fs && P.f_dual_src && uses_src1(s);
    k->fbl = s->fs ? s->feedback : 0;
    if (s->fs && s->color.alpha_func && s->color.alpha_func != NVN_ALPHA_FUNC_ALWAYS)
        k->alpha_func = (uint8_t)(s->color.alpha_func - NVN_ALPHA_FUNC_NEVER + VK_COMPARE_OP_NEVER + 1), k->alpha_ref = s->alpha_ref;
    if (!P.dyn_clamp) k->clamp = s->depth_clamp && P.f_depth_clamp;
    if (!P.dyn_poly) k->poly_mode = (uint8_t)polygon_mode(s->poly.mode);
    if (!P.dyn_a2c) k->a2c = s->ms.a2c != 0;
    if (P.f_logic_op && s->color.logic_op != NVN_LOGIC_OP_COPY) k->logic_op = (uint8_t)(s->color.logic_op + 1);
    for (int i = 0; i < k->n_color; i++) {
        int on = (s->color.blend_en >> i & 1) && blendable(k->color[i]);
        if (!P.dyn_blend_en && on) k->blend_en |= (uint8_t)(1u << i);
        if (!P.dyn_wmask) k->wmask |= target_wmask(s, i) << (4 * i);
        if (!P.dyn_blend_eq && on) {
            const blend_st *b = &s->blend[i];
            uint8_t *o = k->blend[i];
            o[0] = (uint8_t)blend_factor(b->src_rgb), o[1] = (uint8_t)blend_factor(b->dst_rgb), o[2] = (uint8_t)blend_op(b->eq_rgb);
            o[3] = (uint8_t)blend_factor(b->src_a), o[4] = (uint8_t)blend_factor(b->dst_a), o[5] = (uint8_t)blend_op(b->eq_a);
        }
    }
    if (!P.dyn_vi && !k->meshp) {
        VtxInput v;
        vertex_input(s, &v);
        for (uint32_t i = 0; i < v.n_attr; i++) {
            uint32_t l = v.a[i].location;
            k->attr_fmt[l] = v.a[i].format, k->attr_off[l] = v.a[i].offset, k->attr_bind[l] = (uint8_t)v.a[i].binding;
        }
        for (uint32_t i = 0; i < v.n_bind; i++)
            k->rate[v.b[i].binding] = v.b[i].inputRate == VK_VERTEX_INPUT_RATE_INSTANCE ? 2 : 1;
    }
}

// Independent layouts: the outputs the vertex shader lacks from its union with s->fs's inputs added (vs_outs), types in
// fin. The vertex module (and its pre-rasterization library) depends on the vertex shader and this mask only.
uint32_t vs_union_missing(const VkPipeState *s, GfxShaderIo fin[32]) {
    GfxShaderIo vout[32];
    uint32_t need = vs_outs(s->vs, s->fs, fin), have = gfx_shader_outputs(s->vs, vout), missing = 0;
    if (s->fs) {   // a location another fragment shader reads as another type: this pair's type (rare)
        GfxShaderIo fi[32];
        uint32_t fneed = gfx_shader_inputs(s->fs, fi);
        for (int i = 0; i < 32; i++)
            if (fneed >> i & 1 && !fi[i].packed &&
                (!(need >> i & 1) || fin[i].type != fi[i].type || fin[i].comps < fi[i].comps)) {
                if (need >> i & 1 && fin[i].type != fi[i].type)
                    LOG_ONCE("vk_pipeline: varying %d read as type %d and %d by fragment shaders of one vertex shader", i, fin[i].type, fi[i].type);
                fin[i] = fi[i], need |= 1u << i;
            }
    }
    if (P.vs_outs_all)
        for (int i = 0; i < 32; i++)
            if (!(need >> i & 1)) fin[i] = (GfxShaderIo){GFX_TYPE_FLOAT, 4, 0}, need |= 1u << i;
    for (int i = 0; i < 32; i++)
        if (need >> i & 1 && !(have >> i & 1)) missing |= 1u << i;
    return missing;
}

// MKW_FS_SOLID=<hash>[,<hash>...] (diagnosis): fragment shaders with that SPIR-V hash (the dump's sh_<hash>, FNV-1a
// of the module's bytes) are replaced by solid.frag, which writes magenta; "all" replaces every fragment shader.
static int solid_fs(const GfxShader *fs) {
    const char *e = cfg_get("MKW_FS_SOLID");
    if (!e || !*e || !fs) return 0;
    if (!strcmp(e, "all")) return 1;
    uint64_t words = 0, h = 0xcbf29ce484222325ull;
    const uint8_t *b = (const uint8_t *)gfx_shader_spirv(fs, &words);
    for (uint64_t i = 0; i < words * 4; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    char hex[17];
    snprintf(hex, sizeof hex, "%016llx", (unsigned long long)h);
    return strstr(e, hex) != NULL;
}

// Binding offsets of a task/mesh pipeline's stages in its one set: the task program (if any) from 0, the mesh program
// after it, the fragment shader after both, each module's bindings >= 1 moved (spirv_rebind); binding 0 is shared.
void mesh_offsets(const VkPipeState *s, uint32_t *mesh_off, uint32_t *fs_off) {
    uint32_t t = s->mesh ? binding_span(s->vs) - 1 : 0;
    const GfxShader *m = s->mesh ? s->mesh : s->vs;
    *mesh_off = t, *fs_off = t + binding_span(m) - 1;
}

// A task/mesh pipeline: monolithic, no vertex input or input assembly,
// none of the dynamic states a mesh pipeline must not list (VUID-VkGraphicsPipelineCreateInfo-pDynamicStates-07065..07067).
static VkPipeline create_mesh(const VkPipeState *s, const GKey *k, const Layout *l) {
    const GfxShader *task = s->mesh ? s->vs : NULL, *mesh = s->mesh ? s->mesh : s->vs;
    uint32_t mesh_off, fs_off;
    mesh_offsets(s, &mesh_off, &fs_off);
    VkShaderModule mods[3] = {0};
    VkPipelineShaderStageCreateInfo st[3];
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo rq[3];
    uint32_t n = 0;
    VkPipeline pipe = VK_NULL_HANDLE;
    const GfxShader *sh[3] = {task, mesh, s->fs};
    const VkShaderStageFlagBits bits[3] = {VK_SHADER_STAGE_TASK_BIT_EXT, VK_SHADER_STAGE_MESH_BIT_EXT, VK_SHADER_STAGE_FRAGMENT_BIT};
    const uint32_t offs[3] = {0, mesh_off, fs_off};
    for (int i = 0; i < 3; i++) {
        if (!sh[i]) continue;
        uint64_t w;
        const uint32_t *code = gfx_shader_spirv(sh[i], &w);
        uint32_t *moved = offs[i] ? spirv_rebind(code, w, offs[i]) : NULL;
        if (offs[i] && !moved) goto out;
        mods[i] = make_module(moved ? moved : code, w);   // same length
        free(moved);
        if (!mods[i]) goto out;
        st[n] = (VkPipelineShaderStageCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                  .stage = bits[i], .module = mods[i], .pName = "main"};
        pin_subgroup32(&st[n], &rq[n], code, w);
        n++;
    }
    static VkDynamicState dyn[64];
    static uint32_t n_dyn;
    if (!n_dyn)
        for (uint32_t i = 0; i < P.n_dyn; i++)
            if (P.dyn[i] != VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY && P.dyn[i] != VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE &&
                P.dyn[i] != VK_DYNAMIC_STATE_VERTEX_INPUT_EXT && P.dyn[i] != VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE)
                dyn[n_dyn++] = P.dyn[i];
    VkPipelineViewportDepthClipControlCreateInfoEXT clip = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT,
                                                            .negativeOneToOne = k->minus_w};
    VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .pNext = &clip};
    VkPipelineRasterizationProvokingVertexStateCreateInfoEXT pvs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT,
        .provokingVertexMode = VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .pNext = P.pv_last ? &pvs : NULL, .depthClampEnable = k->clamp,
                                                 .polygonMode = (VkPolygonMode)k->poly_mode, .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = (VkSampleCountFlagBits)k->samples,
                                               .alphaToCoverageEnable = k->a2c};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                .maxDepthBounds = 1.0f};
    VkPipelineColorBlendAttachmentState cba[8];
    memset(cba, 0, sizeof cba);
    for (int i = 0; i < k->n_color; i++) {
        const uint8_t *b = k->blend[i];
        cba[i].blendEnable = k->blend_en >> i & 1;
        cba[i].srcColorBlendFactor = (VkBlendFactor)b[0], cba[i].dstColorBlendFactor = (VkBlendFactor)b[1];
        cba[i].colorBlendOp = (VkBlendOp)b[2], cba[i].srcAlphaBlendFactor = (VkBlendFactor)b[3];
        cba[i].dstAlphaBlendFactor = (VkBlendFactor)b[4], cba[i].alphaBlendOp = (VkBlendOp)b[5];
        cba[i].colorWriteMask = k->wmask >> (4 * i) & 0xf;
    }
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .logicOpEnable = k->logic_op != 0,
                                              .logicOp = k->logic_op ? (VkLogicOp)(k->logic_op - 1) : VK_LOGIC_OP_COPY,
                                              .attachmentCount = k->n_color, .pAttachments = cba};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = n_dyn, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = k->n_color,
                                        .pColorAttachmentFormats = (const VkFormat *)k->color,
                                        .depthAttachmentFormat = (VkFormat)k->depth, .stencilAttachmentFormat = (VkFormat)k->stencil};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri,
                                       .stageCount = n, .pStages = st, .pViewportState = &vp, .pRasterizationState = &rs,
                                       .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cb,
                                       .pDynamicState = &dy, .layout = l->layout};
    uint64_t p0 = perf_begin();
    VkResult r = vkCreateGraphicsPipelines(P.device, P.cache, 1, &pi, NULL, &pipe);
    perf_end(PERF_PIPE, p0);
    if (r != VK_SUCCESS) {
        rt_log("vk_pipeline: mesh pipeline creation failed (%d)", (int)r);
        pipe = VK_NULL_HANDLE;
    } else {
        perf_count(PERF_CNT_PIPE_BUILT);
        LOG_ONCE("vk_pipeline: first task/mesh pipeline built (%s)", task ? "task + mesh" : "mesh");
    }
out:
    for (int i = 0; i < 3; i++)
        if (mods[i]) vkDestroyShaderModule(P.device, mods[i], NULL);
    return pipe;
}

// MKW_SPEC_ISA=KEY,KEY (with MKW_SPEC_STATS=1): the driver's internal representations (RADV: NIR, ACO IR and the final
// RDNA assembly) of each executable whose pipeline has a shader key starting with one of those hex prefixes, to
// build/isa/<vs>_<fs>_<executable>.txt: where a translated shader's instructions come from.
static void log_isa(const VkPipelineExecutableInfoKHR *ei, const char *who, const char *exe) {
    static PFN_vkGetPipelineExecutableInternalRepresentationsKHR irs;
    const char *keys = cfg_get("MKW_SPEC_ISA");
    if (!keys || !*keys) return;
    int hit = 0;
    for (const char *k = keys; *k && !hit;) {
        size_t n = strcspn(k, ",");
        char pat[24];
        if (n && n < 17) {
            snprintf(pat, sizeof pat, "=%.*s", (int)n, k);
            hit = strstr(who, pat) != NULL;
        }
        k += n + (k[n] == ',');
    }
    if (!hit) return;
    if (!irs) irs = (PFN_vkGetPipelineExecutableInternalRepresentationsKHR)vkGetDeviceProcAddr(P.device, "vkGetPipelineExecutableInternalRepresentationsKHR");
    VkPipelineExecutableInternalRepresentationKHR ir[8];
    uint32_t nr = 8;
    for (uint32_t i = 0; i < nr; i++) ir[i] = (VkPipelineExecutableInternalRepresentationKHR){.sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INTERNAL_REPRESENTATION_KHR};
    if (!irs || irs(P.device, ei, &nr, ir) < 0) return;
    for (uint32_t i = 0; i < nr; i++) ir[i].pData = ir[i].dataSize ? malloc(ir[i].dataSize) : NULL;
    if (irs(P.device, ei, &nr, ir) >= 0) {
        char path[256], vs[17] = "", fs[17] = "";
        const char *v = strstr(who, "vs="), *f = strstr(who, "fs=");
        if (v) sscanf(v + 3, "%16[0-9a-f]", vs);
        if (f) sscanf(f + 3, "%16[0-9a-f]", fs);
        snprintf(path, sizeof path, "build/isa/%s_%s_%s.txt", vs, fs, exe);
        plat_file_mkdir_p("build/isa");
        FILE *o = fopen(path, "w");
        for (uint32_t i = 0; o && i < nr; i++)
            fprintf(o, "==== %s\n%.*s\n", ir[i].name, ir[i].isText && ir[i].pData ? (int)ir[i].dataSize : 0,
                    ir[i].pData ? (const char *)ir[i].pData : "");
        if (o) fclose(o);
        rt_log("vk_pipeline: isa%s %s -> %s", who, exe, path);
    }
    for (uint32_t i = 0; i < nr; i++) free(ir[i].pData);
}

// MKW_SPEC_STATS=1: one log line per executable of an optimised graphics pipeline with the driver's statistics
// (RADV: instructions, VGPRs, SGPRs, ...) and the shaders' keys, each with the key of its generic shader when it is a
// specialized variant (vk_spec.c): the variant and its generic pipeline compared on the same state. Compute pipelines
// pass their shader as vs and none as fs (vk_pipe_keys.c compute_create).
void log_stats(VkPipeline pipe, const VkPipeState *s) {
    static PFN_vkGetPipelineExecutablePropertiesKHR props;
    static PFN_vkGetPipelineExecutableStatisticsKHR stats;
    if (!props) {
        props = (PFN_vkGetPipelineExecutablePropertiesKHR)vkGetDeviceProcAddr(P.device, "vkGetPipelineExecutablePropertiesKHR");
        stats = (PFN_vkGetPipelineExecutableStatisticsKHR)vkGetDeviceProcAddr(P.device, "vkGetPipelineExecutableStatisticsKHR");
        if (!props || !stats) return;
    }
    char who[200];
    const GfxShader *sh[2] = {s->vs, s->fs};
    int w = 0;
    for (int i = 0; i < 2; i++) {
        const GfxShader *g = sh[i] ? gfx_shader_variant_of(sh[i]) : NULL;
        const uint8_t *a = sh[i] ? gfx_shader_key(sh[i]) : NULL, *b = g ? gfx_shader_key(g) : NULL;
        w += snprintf(who + w, sizeof who - (size_t)w, " %s=", i ? "fs" : "vs");
        for (int j = 0; a && j < 8; j++) w += snprintf(who + w, sizeof who - (size_t)w, "%02x", a[j]);
        if (b) w += snprintf(who + w, sizeof who - (size_t)w, "(of ");
        for (int j = 0; b && j < 8; j++) w += snprintf(who + w, sizeof who - (size_t)w, "%02x", b[j]);
        if (b) w += snprintf(who + w, sizeof who - (size_t)w, ")");
    }
    VkPipelineInfoKHR pinf = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR, .pipeline = pipe};
    VkPipelineExecutablePropertiesKHR ep[8];
    uint32_t ne = 8;
    for (uint32_t i = 0; i < ne; i++) ep[i] = (VkPipelineExecutablePropertiesKHR){.sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR};
    if (props(P.device, &pinf, &ne, ep) < 0) return;
    for (uint32_t e = 0; e < ne; e++) {
        VkPipelineExecutableInfoKHR ei = {.sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR, .pipeline = pipe, .executableIndex = e};
        VkPipelineExecutableStatisticKHR st[32];
        uint32_t ns = 32;
        for (uint32_t i = 0; i < ns; i++) st[i] = (VkPipelineExecutableStatisticKHR){.sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR};
        if (stats(P.device, &ei, &ns, st) < 0) continue;
        char buf[1200];
        int n = 0;
        for (uint32_t i = 0; i < ns && n < (int)sizeof buf - 64; i++) {
            uint64_t v = st[i].format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR ? st[i].value.u64
                       : st[i].format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR ? (uint64_t)st[i].value.i64
                       : st[i].format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR ? st[i].value.b32
                       : (uint64_t)st[i].value.f64;
            n += snprintf(buf + n, sizeof buf - (size_t)n, " %s=%llu", st[i].name, (unsigned long long)v);
        }
        rt_log("vk_pipeline: stats%s %s:%s", who, ep[e].name, buf);
        log_isa(&ei, who, ep[e].name);
    }
}

// MKW_PIPE_STALL=<hex prefix>[:ms] (a test, never set for players): a pipeline or stage library build whose vertex or
// fragment shader key starts with the prefix sleeps ms (60000) before it starts, as a driver build that does not
// return (issue #36). With MKW_PIPE_SYNC=0 a capture replay shows what a waiting draw does then.
void pipe_test_stall(const GfxShader *vs, const GfxShader *fs) {
    static char pre[65];
    static uint32_t ms;
    static int ready = -1;
    if (ready < 0) {
        const char *e = cfg_get("MKW_PIPE_STALL");
        const char *c = e ? strchr(e, ':') : NULL;
        size_t n = e ? (c ? (size_t)(c - e) : strlen(e)) : 0;
        if (n >= sizeof pre) n = sizeof pre - 1;
        memcpy(pre, e ? e : "", n), pre[n] = 0;
        ms = c ? (uint32_t)atoi(c + 1) : 60000;
        ready = n > 0;
    }
    if (!ready) return;
    const GfxShader *sh[2] = {vs, fs};
    for (int i = 0; i < 2; i++) {
        const uint8_t *id = sh[i] ? gfx_shader_key(sh[i]) : NULL;
        char hex[65];
        if (!id) continue;
        for (int j = 0; j < 32; j++) snprintf(hex + 2 * j, 3, "%02x", id[j]);
        if (strncmp(hex, pre, strlen(pre))) continue;
        rt_log("vk_pipeline: MKW_PIPE_STALL: a build with shader %.16s sleeps %u ms", hex, ms);
        plat_sleep_ms(ms);
        return;
    }
}

VkPipeline create_graphics(const VkPipeState *s, const GKey *k, const Layout *l, uint32_t fs_off) {
    if (k->meshp) return create_mesh(s, k, l);
    pipe_test_stall(s->vs, s->fs);
    VkShaderModule vm = VK_NULL_HANDLE, fm = VK_NULL_HANDLE;
    uint64_t vw, fw;
    const uint32_t *vcode = gfx_shader_spirv(s->vs, &vw);
    uint32_t *vpatched = NULL, *fpatched = NULL;
    if (l->set1) {   // independent layouts: outputs from the vertex shader's union, not from this fragment shader
        GfxShaderIo fin[32];
        uint32_t missing = vs_union_missing(s, fin);
        if (missing) {
            uint64_t n2 = 0;
            vpatched = spirv_add_outputs(vcode, vw, missing, fin, &n2);
            if (!vpatched) LOG_ONCE("vk_pipeline: cannot add missing vertex outputs (mask %#x)", missing);
            else vcode = vpatched, vw = n2;
        }
    }
    if (s->fs) {
        GfxShaderIo fin[32], vout[32];
        uint32_t need = gfx_shader_inputs(s->fs, fin), have = gfx_shader_outputs(s->vs, vout), missing = 0;
        for (int i = 0; i < 32; i++) {
            if (!(need >> i & 1) || fin[i].packed) continue;
            if (!(have >> i & 1)) missing |= 1u << i;
            else if (vout[i].type != fin[i].type)
                LOG_ONCE("vk_pipeline: varying %d is type %d in the vertex stage, %d in the fragment stage", i, vout[i].type, fin[i].type);
        }
        if (missing && !l->set1) {
            uint64_t n2 = 0;
            vpatched = spirv_add_outputs(vcode, vw, missing, fin, &n2);
            if (!vpatched) LOG_ONCE("vk_pipeline: cannot add missing vertex outputs (mask %#x)", missing);
            else vcode = vpatched, vw = n2;
        }
        const uint32_t *fcode = gfx_shader_spirv(s->fs, &fw);
        if (k->dsb) {
            uint64_t n2 = 0;
            uint32_t *d = spirv_dual_source(fcode, fw, &n2);
            if (d) fcode = fpatched = d, fw = n2;
            else LOG_ONCE("vk_pipeline: SRC1 blend factors but the fragment shader writes no colour output 1");
        }
        if (k->alpha_func && fcode) {
            uint64_t n2 = 0;
            uint32_t *d = spirv_alpha_test(fcode, fw, k->alpha_func - 1, k->alpha_ref, &n2);
            if (d) free(fpatched), fcode = fpatched = d, fw = n2;
            else LOG_ONCE("vk_pipeline: alpha test on a fragment shader without a float vec4 colour output 0");
        }
        if (l->set1 || fs_off) {
            uint32_t *r = !fcode ? NULL : l->set1 ? spirv_set(fcode, fw, 1) : spirv_rebind(fcode, fw, fs_off);
            free(fpatched);
            fcode = fpatched = r;
        }
        // A bound colour target outside the shader's output map gets write mask 0 (target_wmask)
        if (fcode && solid_fs(s->fs)) {   // MKW_FS_SOLID diagnosis
            free(fpatched), fpatched = NULL;
            fcode = solid_spv, fw = sizeof solid_spv / 4;
        }
        if (fcode) fm = make_module(fcode, fw);
    }
    vm = make_module(vcode, vw);
    free(vpatched), free(fpatched);
    VkPipeline pipe = VK_NULL_HANDLE;
    if (!vm || (s->fs && !fm)) goto out;

    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vm, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fm, .pName = "main"}};
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo rqv, rqf;
    uint64_t sw;
    const uint32_t *scode = gfx_shader_spirv(s->vs, &sw);   // vcode may be a patched copy that is freed by now
    pin_subgroup32(&stages[0], &rqv, scode, sw);
    if (s->fs) pin_subgroup32(&stages[1], &rqf, gfx_shader_spirv(s->fs, &sw), sw);
    VkVertexInputAttributeDescription va[32];
    VkVertexInputBindingDescription vb[17];
    uint32_t na = 0, nb = 0;
    if (!P.dyn_vi) {
        VtxInput v;
        vertex_input(s, &v);
        for (uint32_t i = 0; i < v.n_attr; i++) va[na++] = (VkVertexInputAttributeDescription){v.a[i].location, v.a[i].binding, v.a[i].format, v.a[i].offset};
        for (uint32_t i = 0; i < v.n_bind; i++) vb[nb++] = (VkVertexInputBindingDescription){v.b[i].binding, v.b[i].stride, v.b[i].inputRate};
    }
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                               .vertexBindingDescriptionCount = nb, .pVertexBindingDescriptions = vb,
                                               .vertexAttributeDescriptionCount = na, .pVertexAttributeDescriptions = va};
    static const VkPrimitiveTopology class_base[4] = {VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
                                                      VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_PATCH_LIST};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = class_base[k->topo_class]};
    VkPipelineViewportDepthClipControlCreateInfoEXT clip = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT,
                                                            .negativeOneToOne = k->minus_w};
    VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .pNext = &clip};
    // NVN's default is the last vertex (SetProvokingVertexMode, nvn_ext.c); Vulkan's is the first.
    VkPipelineRasterizationProvokingVertexStateCreateInfoEXT pvs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT,
        .provokingVertexMode = VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .pNext = P.pv_last ? &pvs : NULL,
                                                 .depthClampEnable = k->clamp, .polygonMode = (VkPolygonMode)k->poly_mode,
                                                 .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = (VkSampleCountFlagBits)k->samples,
                                               .alphaToCoverageEnable = k->a2c};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                .maxDepthBounds = 1.0f};
    VkPipelineColorBlendAttachmentState cba[8];
    memset(cba, 0, sizeof cba);
    for (int i = 0; i < k->n_color; i++) {
        const uint8_t *b = k->blend[i];
        cba[i].blendEnable = k->blend_en >> i & 1;
        cba[i].srcColorBlendFactor = (VkBlendFactor)b[0], cba[i].dstColorBlendFactor = (VkBlendFactor)b[1];
        cba[i].colorBlendOp = (VkBlendOp)b[2], cba[i].srcAlphaBlendFactor = (VkBlendFactor)b[3];
        cba[i].dstAlphaBlendFactor = (VkBlendFactor)b[4], cba[i].alphaBlendOp = (VkBlendOp)b[5];
        cba[i].colorWriteMask = k->wmask >> (4 * i) & 0xf;
    }
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .logicOpEnable = k->logic_op != 0,
                                              .logicOp = k->logic_op ? (VkLogicOp)(k->logic_op - 1) : VK_LOGIC_OP_COPY,
                                              .attachmentCount = k->n_color, .pAttachments = cba};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = P.n_dyn, .pDynamicStates = P.dyn};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = k->n_color,
                                        .pColorAttachmentFormats = (const VkFormat *)k->color,
                                        .depthAttachmentFormat = (VkFormat)k->depth, .stencilAttachmentFormat = (VkFormat)k->stencil};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri,
                                       .stageCount = s->fs ? 2 : 1, .pStages = stages, .pVertexInputState = &vi,
                                       .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
                                       .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cb,
                                       .pDynamicState = &dy, .layout = l->layout};
    if (k->fbl & 1) pi.flags |= VK_PIPELINE_CREATE_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;
    if (k->fbl & 2) pi.flags |= VK_PIPELINE_CREATE_DEPTH_STENCIL_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;
    if (vk_ctx()->has_exec_stats) pi.flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
    if (vk_ctx()->has_exec_stats && cfg_get("MKW_SPEC_ISA")) pi.flags |= VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR;
    uint64_t p0 = perf_begin();
    VkResult r = vkCreateGraphicsPipelines(P.device, P.cache, 1, &pi, NULL, &pipe);
    perf_end(PERF_PIPE, p0);   // counts on the render thread only, so background builds stay out of it
    if (r != VK_SUCCESS) {
        rt_log("vk_pipeline: vkCreateGraphicsPipelines failed (%d)", (int)r);
        pipe = VK_NULL_HANDLE;
    } else {
        perf_count(PERF_CNT_PIPE_BUILT);
        if (vk_ctx()->has_exec_stats) log_stats(pipe, s);
    }
out:
    if (vm) vkDestroyShaderModule(P.device, vm, NULL);
    if (fm) vkDestroyShaderModule(P.device, fm, NULL);
    return pipe;
}

// ---- background graphics pipeline builds (vk_pipeline_async_enable)
// A miss in vk_pipeline_graphics_async enters the Pipe as building and queues a job; the replay skips draws with it
// until a worker publishes the handle. The job holds copies of the state and the key. What they point to outlives it:
// shaders (gfx_shader_get results) live for the process, layouts until vk_pipeline_shutdown, which drains the queue first. Workers call vkCreateGraphicsPipelines
// without P.lock: P.cache is created without VK_PIPELINE_CACHE_CREATE_EXTERNALLY_SYNCHRONIZED_BIT, so the driver
// synchronises it, vkGetPipelineCacheData in vk_pipeline_save included. Compute pipelines stay synchronous: a skipped
// dispatch can change what the game computes.
//
// Two queues: draws' misses (A.head) and the boot prebuild (A.low, see "pipeline keys" below). Workers take the low
// queue only when the other is empty and run its jobs at PLAT_PRIO_BELOW_NORMAL, so guest threads win. A draw
// that finds a prebuild job still queued moves it to the draw queue; one that finds it running raises its worker.

AsyncBuilds A = {.idle = PLAT_COND_INIT};

// ---- pipeline keys and the boot prewarm: vk_pipe_keys.c

// ---- dynamic state

static void blend_arrays(const VkPipeState *s, int n, VkBool32 *en, VkColorBlendEquationEXT *eq, VkColorComponentFlags *wm) {
    for (int i = 0; i < n; i++) {
        const blend_st *b = &s->blend[i];
        en[i] = (s->color.blend_en >> i & 1) && blendable(target_format(s->color_fmt[i], NULL, NULL));
        eq[i] = (VkColorBlendEquationEXT){blend_factor(b->src_rgb), blend_factor(b->dst_rgb), blend_op(b->eq_rgb),
                                          blend_factor(b->src_a), blend_factor(b->dst_a), blend_op(b->eq_a)};
        wm[i] = target_wmask(s, i);
    }
}

static unsigned offset_enabled(const poly_st *pl) {
    // NVN_POLYGON_OFFSET_ENABLE_* bit of the polygon mode in use: point 1, line 2, fill 4
    unsigned mode = pl->mode <= NVN_POLYGON_MODE_FILL ? pl->mode : NVN_POLYGON_MODE_FILL;
    return pl->offset_en >> mode & 1;
}

// last: the state this command buffer's dynamic state was set from (vk_exec_draw.c keeps it across pipeline binds, as
// every graphics pipeline lists the same dynamic states, P.dyn); each vkCmdSet* is recorded only when its value
// differs from what last gave. NULL: all of them. A race frame on the Steam Deck (1.8.0 Grand Prix, 2 659 draws) made
// 19 643 of these calls, of which 1 385 changed a value (MKW_DYN_CENSUS, a test build).
void vk_pipeline_set_dynamic(VkCommandBuffer cb, const VkPipeState *s, const VkPipeState *last) {
    if (!ensure_init()) return;
    // a task/mesh pipeline lists fewer dynamic states (create_mesh): binding one between two others leaves those
    // undefined, so a change of kind sets them all
    if (last && mesh_pipe(s) != mesh_pipe(last)) last = NULL;
    const poly_st *pl = &s->poly, *lp = last ? &last->poly : NULL;
    const ds_st *d = &s->ds, *ld = last ? &last->ds : NULL;
#define CHANGED(cond) (!last || (cond))
    // The front face maps unchanged: TotK sets the upper-left window origin, and the y-flipped viewport (y_flip) puts
    // NDC +y at row 0 as NVN does, so the framebuffer winding Vulkan tests is the one NVN tests (MacNativeGames passes
    // it to Metal unchanged; LAS Recomp exec.c mirrors it only for the lower-left origin).
    int front_cw = pl->front == NVN_FRONT_FACE_CW;
    if (CHANGED((pl->cull & 3) != (lp->cull & 3)))
        vkCmdSetCullMode(cb, (VkCullModeFlags)(pl->cull & 3));   // NVN_FACE_* has VkCullModeFlagBits' values
    if (CHANGED(front_cw != (lp->front == NVN_FRONT_FACE_CW)))
        vkCmdSetFrontFace(cb, front_cw ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE);
    if (CHANGED(vk_pipeline_topology(s->prim) != vk_pipeline_topology(last->prim)))
        vkCmdSetPrimitiveTopology(cb, vk_pipeline_topology(s->prim));
    if (CHANGED(d->dt != ld->dt)) vkCmdSetDepthTestEnable(cb, d->dt);
    if (CHANGED(d->dw != ld->dw)) vkCmdSetDepthWriteEnable(cb, d->dw);
    if (CHANGED(compare_op(d->df) != compare_op(ld->df))) vkCmdSetDepthCompareOp(cb, compare_op(d->df));
    if (CHANGED(d->st != ld->st)) vkCmdSetStencilTestEnable(cb, d->st);
    if (CHANGED(d->fail_f != ld->fail_f || d->zpass_f != ld->zpass_f || d->zfail_f != ld->zfail_f || d->sf_f != ld->sf_f))
        vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_BIT, stencil_op(d->fail_f), stencil_op(d->zpass_f),
                          stencil_op(d->zfail_f), compare_op(d->sf_f));
    if (CHANGED(d->fail_b != ld->fail_b || d->zpass_b != ld->zpass_b || d->zfail_b != ld->zfail_b || d->sf_b != ld->sf_b))
        vkCmdSetStencilOp(cb, VK_STENCIL_FACE_BACK_BIT, stencil_op(d->fail_b), stencil_op(d->zpass_b),
                          stencil_op(d->zfail_b), compare_op(d->sf_b));
    if (CHANGED(offset_enabled(pl) != offset_enabled(lp))) vkCmdSetDepthBiasEnable(cb, offset_enabled(pl) != 0);
    if (CHANGED((s->raster_discard != 0) != (last->raster_discard != 0)))
        vkCmdSetRasterizerDiscardEnable(cb, s->raster_discard != 0);
    if (CHANGED((s->prim_restart != 0) != (last->prim_restart != 0)))
        vkCmdSetPrimitiveRestartEnable(cb, s->prim_restart != 0);
    // vertex_input reads the vertex shader's inputs, vattr and vstream: equal ones give the same description; other
    // ones may too (another shader with the same inputs), compared on the descriptions themselves
    if (P.dyn_vi && s->vs &&
        CHANGED(last->vs != s->vs || memcmp(s->vattr, last->vattr, sizeof s->vattr) ||
                memcmp(s->vstream, last->vstream, sizeof s->vstream))) {
        VtxInput v, lv;
        vertex_input(s, &v);   // it clears *v first
        int same = 0;
        if (last && last->vs) vertex_input(last, &lv), same = !memcmp(&v, &lv, sizeof v);
        if (!same) rec_vertex_input(P.set_vi, cb, v.n_bind, v.b, v.n_attr, v.a);
    }
    if (P.dyn_clamp && CHANGED((s->depth_clamp != 0) != (last->depth_clamp != 0)))
        rec_bool(P.set_clamp, cb, s->depth_clamp != 0);
    if (P.dyn_poly && CHANGED(pl->mode != lp->mode)) rec_polygon_mode(P.set_poly, cb, polygon_mode(pl->mode));
    if (P.dyn_a2c && CHANGED((s->ms.a2c != 0) != (last->ms.a2c != 0)))
        rec_bool((PFN_vkCmdSetDepthClampEnableEXT)P.set_a2c, cb, s->ms.a2c != 0);   // the same signature
    int n = target_count(s);
    if (!n) return;
    VkBool32 en[8];
    VkColorBlendEquationEXT eq[8];
    VkColorComponentFlags wm[8];
    blend_arrays(s, n, en, eq, wm);
    // the last state's targets: a target it did not have was never set, so a count above its own sets them all
    int ln = last ? target_count(last) : 0;
    VkBool32 len[8];
    VkColorBlendEquationEXT leq[8];
    VkColorComponentFlags lwm[8];
    if (ln >= n) blend_arrays(last, n, len, leq, lwm);
    if (P.dyn_blend_en && (ln < n || memcmp(en, len, n * sizeof *en))) rec_blend_enable(P.set_blend_en, cb, 0, (uint32_t)n, en);
    if (P.dyn_blend_eq && (ln < n || memcmp(eq, leq, n * sizeof *eq))) rec_blend_equation(P.set_blend_eq, cb, 0, (uint32_t)n, eq);
    if (P.dyn_wmask && (ln < n || memcmp(wm, lwm, n * sizeof *wm))) rec_write_mask(P.set_wmask, cb, 0, (uint32_t)n, wm);
#undef CHANGED
}

// ---- statistics, persistence, teardown

void vk_pipeline_stats(VkPipeStats *s) {
    plat_mutex_lock_shared(&P.lock);
    *s = P.stats;
    plat_mutex_unlock_shared(&P.lock);
}

static PlatMutex save_lock = PLAT_MUTEX_INIT;   // one cache save at a time; shutdown takes it to destroy the cache

void vk_pipeline_save(void) {
    if (P.ready) keys_flush();
    if (P.ready && P.gpl) {
        rt_log("vk_pipeline: gpl: %llu fast links (%llu by workers; avg %.2f ms, max %.2f ms, %llu failed), %llu draws "
               "lookups found shader libraries missing (left to a worker), %llu optimised pipelines swapped in (%llu queued "
               "low after their fast link), %llu drawing-thread links over 5 ms; %llu waiting draws built their libraries "
               "(avg %.2f ms, max %.2f ms)",
               (unsigned long long)P.g.links, (unsigned long long)P.g.wlinks, P.g.links ? P.g.link_ms / P.g.links : 0.0,
               P.g.link_max, (unsigned long long)P.g.link_fail, (unsigned long long)P.g.defer,
               (unsigned long long)P.g.swaps, (unsigned long long)P.g.requeued, (unsigned long long)P.g.slow,
               (unsigned long long)P.g.dlinks, P.g.dlinks ? P.g.dlink_ms / P.g.dlinks : 0.0, P.g.dlink_max);
        if (U.waits || U.queued)
            rt_log("vk_pipeline: gpl: library jobs for waiting draws: %llu queued, %llu built by a worker, %llu by a "
                   "helper thread (no worker free), %llu waits ended at their limit",
                   (unsigned long long)U.queued, (unsigned long long)U.by_worker, (unsigned long long)U.stolen,
                   (unsigned long long)U.timeouts);
        static const char *const kn[4] = {"vertex input", "pre-rasterization", "fragment shader", "fragment output"};
        for (int i = 0; i < 4; i++)
            rt_log("vk_pipeline: gpl: %s libraries %llu (avg %.2f ms, max %.2f ms)", kn[i], (unsigned long long)P.g.lib_n[i],
                   P.g.lib_n[i] ? P.g.lib_ms[i] / P.g.lib_n[i] : 0.0, P.g.lib_max[i]);
    }
    if (!P.ready || !P.cache || !P.cache_path[0]) return;
    // Not under P.lock: the driver synchronises P.cache, and holding P.lock through the copy and the file write (about
    // 60 ms on the 1.4.3 fight) stopped every pipeline lookup of the replay, a freeze at each 20 s autosave.
    plat_mutex_lock(&save_lock);
    if (!P.cache) {   // vk_pipeline_shutdown destroyed it
        plat_mutex_unlock(&save_lock);
        return;
    }
    size_t size = 0;
    void *data = NULL;
    if (vkGetPipelineCacheData(P.device, P.cache, &size, NULL) == VK_SUCCESS && size && (data = malloc(size)) &&
        vkGetPipelineCacheData(P.device, P.cache, &size, data) == VK_SUCCESS) {
        if (!plat_file_write_atomic(P.cache_path, data, size)) rt_log("vk_pipeline: cannot write %s", P.cache_path);
    }
    free(data);
    plat_mutex_unlock(&save_lock);
}

// The game never shuts the device down (boot.c and the window end the process), so the cache is saved while it
// runs: at most every 20 s, and only after new pipelines were built. New pipeline keys are appended every 2 s.
static void autosave_once(void) {
    static uint64_t last_n, last_ms, last_keys;
    if (!P.ready) return;
    uint64_t now = plat_now_ms();
    if (*(volatile uint32_t *)&K.pend_n && now - last_keys >= 2000) last_keys = now, keys_flush();
    if (now - last_ms < 20000) return;
    VkPipeStats s;
    vk_pipeline_stats(&s);
    if (s.graphics + s.compute == last_n) return;
    last_n = s.graphics + s.compute, last_ms = now;
    vk_pipeline_save();
}

// On its own thread: the save (vkGetPipelineCacheData and the file write, about 60 ms) and the key appends (a flush to
// the disk each) ran on the present thread, and the render thread waited for it to take the next frame.
static void autosave_main(void *arg) {
    (void)arg;
    plat_thread_priority(PLAT_PRIO_BELOW_NORMAL);
    for (;;) {
        plat_sleep_ms(500);
        autosave_once();
    }
}

void vk_pipeline_autosave(void) {
    static int32_t started;
    if (!P.ready || plat_xchg32(&started, 1)) return;
    if (!plat_thread_spawn(autosave_main, NULL, "pipe autosave", 0))
        rt_log("vk_pipeline: cannot start the autosave thread; the cache is saved at exit only");
}

void vk_pipeline_shutdown(void) {
    if (!P.ready) return;
    workers_drain();   // jobs point at layouts and Pipe entries freed below
    vk_pipeline_save();
    plat_mutex_lock(&P.lock);
    vkDeviceWaitIdle(P.device);
    for (int i = 0; i < PIPE_BUCKETS; i++) {
        for (Pipe *p = pipes[i], *n; p; p = n) {
            n = p->next;
            if (p->info.pipeline) vkDestroyPipeline(P.device, p->info.pipeline, NULL);
            if (p->fast && p->fast != p->info.pipeline) vkDestroyPipeline(P.device, p->fast, NULL);
            free(p->bindings), free(p);
        }
        pipes[i] = NULL;
    }
    libs_destroy();
    for (int i = 0; i < LAYOUT_BUCKETS; i++) {
        for (Layout *l = layouts[i], *n; l; l = n) {
            n = l->next;
            for (int k = 0; k < 2; k++)
                if (l->tpl[k]) P.tpl_destroy(P.device, l->tpl[k], NULL);
            vkDestroyPipelineLayout(P.device, l->layout, NULL);
            for (int h = 0; h < 2; h++)
                if (l->half[h]) vkDestroyPipelineLayout(P.device, l->half[h], NULL);
            vkDestroyDescriptorSetLayout(P.device, l->set, NULL);
            if (l->set1) vkDestroyDescriptorSetLayout(P.device, l->set1, NULL);
            free(l);
        }
        layouts[i] = NULL;
    }
    plat_mutex_lock(&save_lock);   // not while the autosave thread copies it
    if (P.cache) vkDestroyPipelineCache(P.device, P.cache, NULL);
    P.cache = VK_NULL_HANDLE;
    plat_mutex_unlock(&save_lock);
    memset(&P.stats, 0, sizeof P.stats);
    plat_xchg32(&P.ready, 0);
    plat_mutex_unlock(&P.lock);
}
