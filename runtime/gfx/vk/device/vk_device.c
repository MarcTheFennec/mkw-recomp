// Vulkan instance, physical device choice, logical device, VMA, timeline semaphore and the validation messenger.
// Ported from LAS Recomp, runtime/nvn/vulkan/device.c:33-330
// (https://github.com/peiikai/LASRecomp @ da35b83a9611d046a9a681aae668d7085954d37f, MIT, Copyright (c) 2026 Peiikai),
// without SDL. The VMA implementation is runtime/vk_vma.cpp (VMA is C++ only).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <pthread.h>
#endif

#include "cfg.h"
#include "host.h"
#include "vk.h"
#include "vk_defaults.h"
#include "platform/platform.h"

static VkCtx g_vk;
static int g_ready;
static PlatMutex g_lock = PLAT_MUTEX_INIT;

// VMA's vkAllocateMemory, timed: a new VkDeviceMemory costs the calling thread 8-19 ms or more (the kernel clears its
// pages; vk_headroom.c), and the title's sparse arrays ask for ~1 GB in one frame. A slow one (5 ms or more) is logged
// with its thread, the first 8 and then every power of two.
static VKAPI_ATTR VkResult VKAPI_CALL timed_alloc(VkDevice d, const VkMemoryAllocateInfo *info,
                                                  const VkAllocationCallbacks *cb, VkDeviceMemory *mem) {
    static int64_t n_slow, ns_all, bytes_all;
    uint64_t t0 = plat_now_ns();
    VkResult r = vkAllocateMemory(d, info, cb, mem);
    int64_t dt = (int64_t)(plat_now_ns() - t0);
    int64_t all = plat_add64(&ns_all, dt), bytes = plat_add64(&bytes_all, (int64_t)info->allocationSize);
    if (dt >= 5000000) {
        int64_t k = plat_inc64(&n_slow);
        char name[32] = "";
#ifndef _WIN32
        pthread_getname_np(pthread_self(), name, sizeof name);
#endif
        if (k <= 8 || !(k & (k - 1)))
            rt_log("vk: vkAllocateMemory of %.1f MiB (type %u) took %.1f ms on thread %u %s (%lld slow; %.0f ms for "
                   "%.0f MiB in all so far)", info->allocationSize / 1048576.0, info->memoryTypeIndex, dt / 1e6,
                   plat_thread_id(), name, (long long)k, all / 1e6, bytes / 1048576.0);
    }
    return r;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT type,
                                               const VkDebugUtilsMessengerCallbackDataEXT *d, void *user) {
    (void)type, (void)user;
    int err = (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0;
    if (err) plat_inc32(&g_vk.validation_errors);
    // The first 3 messages of each VUID, however many VUIDs: a plain cap (500) was used up during loading and hid what
    // went wrong later in the race (run-race17).
    static PlatMutex vl = PLAT_MUTEX_INIT;
    static struct { char id[96]; int n; } seen[512];
    static int nseen;
    // Per VUID and named object (set_name: the program key of a pipeline), so each program of a VUID shows up.
    char id[96], names[160] = "";
    size_t nl = 0;
    for (uint32_t i = 0; i < d->objectCount && nl + 1 < sizeof names; i++)
        if (d->pObjects[i].pObjectName)
            nl += (size_t)snprintf(names + nl, sizeof names - nl, " [%s]", d->pObjects[i].pObjectName);
    snprintf(id, sizeof id, "%s%s", d->pMessageIdName ? d->pMessageIdName : "?", names);
    plat_mutex_lock(&vl);
    int k = 0;
    while (k < nseen && strcmp(seen[k].id, id)) k++;
    if (k == nseen && nseen < 512) snprintf(seen[nseen++].id, sizeof seen[0].id, "%s", id);
    int show = k < 512 && ++seen[k].n <= 3;
    plat_mutex_unlock(&vl);
    if (show) rt_log("vk validation %s:%s %s", err ? "error" : "warning", names, d->pMessage);
    return VK_FALSE;
}

static int has_ext(const VkExtensionProperties *e, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return 1;
    return 0;
}

static int layer_present(const char *name) {
    uint32_t n = 0;
    vkEnumerateInstanceLayerProperties(&n, NULL);
    VkLayerProperties *l = calloc(n ? n : 1, sizeof *l);
    vkEnumerateInstanceLayerProperties(&n, l);
    int found = 0;
    for (uint32_t i = 0; i < n; i++) if (!strcmp(l[i].layerName, name)) found = 1;
    free(l);
    return found;
}

static int create_instance(void) {
    const char *ext[9];
    uint32_t n_ext = (uint32_t)gfx_window_vk_extensions(ext, 7);
    uint32_t ni = 0;   // HDR10 swapchains (gfx_hdr.c) need the colour spaces of VK_EXT_swapchain_colorspace
    vkEnumerateInstanceExtensionProperties(NULL, &ni, NULL);
    VkExtensionProperties *ie = calloc(ni ? ni : 1, sizeof *ie);
    vkEnumerateInstanceExtensionProperties(NULL, &ni, ie);
    if (has_ext(ie, ni, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME))
        g_vk.has_colorspace = 1, ext[n_ext++] = VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME;
    free(ie);
    const char *layers[] = {"VK_LAYER_KHRONOS_validation"};
    int validate = cfg_bool("MKW_VK_VALIDATION");
    if (validate && !layer_present(layers[0])) {
        rt_log("vk: MKW_VK_VALIDATION set but %s is not installed; continuing without", layers[0]);
        validate = 0;
    }
    if (validate) ext[n_ext++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "MKW Recomp",
                             .pEngineName = "MKW Recomp", .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                               .enabledExtensionCount = n_ext, .ppEnabledExtensionNames = ext,
                               .enabledLayerCount = validate ? 1 : 0, .ppEnabledLayerNames = layers};
    rdoc_init();
    VkResult r = vkCreateInstance(&ci, NULL, &g_vk.instance);
    if (r != VK_SUCCESS) { rt_log("vk: vkCreateInstance failed (%d); is a Vulkan 1.3 driver installed?", (int)r); return 0; }
    g_vk.validation = validate;
    if (validate) {
        PFN_vkCreateDebugUtilsMessengerEXT f =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_vk.instance, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT mi = {
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .pfnUserCallback = debug_cb};
        if (f) f(g_vk.instance, &mi, NULL, &g_vk.messenger);
        g_vk.set_name = (PFN_vkSetDebugUtilsObjectNameEXT)vkGetInstanceProcAddr(g_vk.instance, "vkSetDebugUtilsObjectNameEXT");
    }
    return 1;
}

// VK_EXT_external_memory_host is optional (has_emh): pools are shadowed by default and imported only when a shadow
// does not fit (vk_mem_back.c import_pool). Turnip (Adreno, Mesa 25-27) does not offer it.
static const char *const k_required_ext[] = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME};

// Name of the first missing required extension, or NULL.
static const char *missing_required(VkPhysicalDevice pd) {
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &n, NULL);
    VkExtensionProperties *e = calloc(n ? n : 1, sizeof *e);
    vkEnumerateDeviceExtensionProperties(pd, NULL, &n, e);
    const char *miss = NULL;
    for (size_t i = 0; i < sizeof k_required_ext / sizeof *k_required_ext && !miss; i++)
        if (!has_ext(e, n, k_required_ext[i])) miss = k_required_ext[i];
    free(e);
    return miss;
}

// Graphics+compute family that can present to the window.
static uint32_t find_gfx_family(VkPhysicalDevice pd, const VkQueueFamilyProperties *qp, uint32_t nq) {
    for (uint32_t i = 0; i < nq; i++)
        if ((qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) &&
            gfx_window_vk_presentable(g_vk.instance, pd, i))
            return i;
    return UINT32_MAX;
}

static int name_has(const char *s, const char *sub) {
    for (size_t n = strlen(sub); *s; s++)
        if (!strncasecmp(s, sub, n)) return 1;
    return 0;
}

// First discrete GPU with Vulkan 1.3, the required extensions and a graphics+present queue; MKW_GPU_INDEX (index) or
// MKW_GPU_NAME (case-insensitive part of the name) overrides.
static int pick_device(void) {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(g_vk.instance, &n, NULL);
    if (!n) { rt_log("vk: no Vulkan device"); return 0; }
    VkPhysicalDevice *pd = calloc(n, sizeof *pd);
    vkEnumeratePhysicalDevices(g_vk.instance, &n, pd);
    const char *e = cfg_get("MKW_GPU_INDEX");   // no fixed default: the best device when unset
    int want = e && *e ? atoi(e) : -1;
    const char *name = cfg_get("MKW_GPU_NAME");
    int best = -1, best_score = -1;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd[i], &p);
        rt_log("vk: gpu %u: %s (type %d, api %u.%u.%u)", i, p.deviceName, (int)p.deviceType, VK_API_VERSION_MAJOR(p.apiVersion),
               VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion));
        if (want >= 0 && (int)i != want) continue;
        if (name && *name && !name_has(p.deviceName, name)) continue;
        if (p.apiVersion < VK_API_VERSION_1_3) { rt_log("vk: gpu %u skipped: Vulkan below 1.3", i); continue; }
        const char *miss = missing_required(pd[i]);
        if (miss) { rt_log("vk: gpu %u skipped: missing extension %s", i, miss); continue; }
        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd[i], &nq, NULL);
        VkQueueFamilyProperties *qp = calloc(nq ? nq : 1, sizeof *qp);
        vkGetPhysicalDeviceQueueFamilyProperties(pd[i], &nq, qp);
        uint32_t fam = find_gfx_family(pd[i], qp, nq);
        free(qp);
        if (fam == UINT32_MAX) { rt_log("vk: gpu %u skipped: no graphics+compute+present queue family", i); continue; }
        int score = (int)i == want ? 100 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3
                  : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
        if (score > best_score) { best_score = score; best = (int)i; }
    }
    if (best < 0) {
        if (want >= 0) rt_log("vk: MKW_GPU_INDEX=%d is not usable (see above)", want);
        else if (name && *name) rt_log("vk: no usable gpu matches MKW_GPU_NAME=%s (see above)", name);
        else rt_log("vk: no device with Vulkan 1.3 and the required extensions (see above)");
        free(pd);
        return 0;
    }
    g_vk.phys = pd[best];
    g_vk.device_index = best;
    free(pd);
    vkGetPhysicalDeviceProperties(g_vk.phys, &g_vk.props);
    vkGetPhysicalDeviceMemoryProperties(g_vk.phys, &g_vk.mem_props);
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &nq, NULL);
    VkQueueFamilyProperties *qp = calloc(nq ? nq : 1, sizeof *qp);
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &nq, qp);
    g_vk.gfx_family = find_gfx_family(g_vk.phys, qp, nq);
    g_vk.xfer_family = g_vk.gfx_family;
    for (uint32_t i = 0; i < nq; i++)   // a family that does transfer only (async DMA engine)
        if ((qp[i].queueFlags & VK_QUEUE_TRANSFER_BIT) && !(qp[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))) {
            g_vk.xfer_family = i;
            break;
        }
    free(qp);
    rt_log("vk: using gpu %d: %s", best, g_vk.props.deviceName);
    vk_mem_class_log(&g_vk.props, &g_vk.mem_props);
    return 1;
}

typedef struct {
    VkPhysicalDeviceFeatures2 f2;
    VkPhysicalDeviceVulkan11Features f11;
    VkPhysicalDeviceVulkan12Features f12;
    VkPhysicalDeviceVulkan13Features f13;
    VkPhysicalDeviceDepthClipControlFeaturesEXT clip;
    VkPhysicalDeviceCustomBorderColorFeaturesEXT cbc;
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT eds;
    VkPhysicalDeviceExtendedDynamicState2FeaturesEXT eds2;
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3;
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT vids;
    VkPhysicalDeviceConditionalRenderingFeaturesEXT cond;
    VkPhysicalDeviceTransformFeedbackFeaturesEXT xfb;
    VkPhysicalDeviceProvokingVertexFeaturesEXT pv;
    VkPhysicalDeviceRobustness2FeaturesEXT rob2;
    VkPhysicalDeviceFaultFeaturesEXT fault;
    VkPhysicalDeviceMeshShaderFeaturesEXT mesh;
    VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT unused;
    VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT fsi;
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gpl;
    VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT fbl;
    VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR lr;
    VkPhysicalDeviceMapMemoryPlacedFeaturesEXT placed;
    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR pexe;
    VkPhysicalDevicePresentIdFeaturesKHR pid;
    VkPhysicalDevicePresentWaitFeaturesKHR pwait;
} FeatureChain;

static int create_device(void) {
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(g_vk.phys, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne ? ne : 1, sizeof *ep);
    vkEnumerateDeviceExtensionProperties(g_vk.phys, NULL, &ne, ep);
    const char *ext[48];   // every optional extension at once nears 32
    uint32_t n_ext = 0;
    for (size_t i = 0; i < sizeof k_required_ext / sizeof *k_required_ext; i++) ext[n_ext++] = k_required_ext[i];

    FeatureChain *c = calloc(1, sizeof *c);
    void **tail = &c->f2.pNext;
#define CHAIN(member, stype) do { c->member.sType = stype; *tail = &c->member; tail = &c->member.pNext; } while (0)
    c->f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    CHAIN(f11, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES);
    CHAIN(f12, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
    CHAIN(f13, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);
    CHAIN(clip, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT);
#define OPTIONAL_EXT(flag, name, member, stype) do { \
        if (has_ext(ep, ne, name)) { g_vk.flag = 1; ext[n_ext++] = name; CHAIN(member, stype); } } while (0)
    OPTIONAL_EXT(has_custom_border_color, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME, cbc, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT);
    OPTIONAL_EXT(has_eds, VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME, eds, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT);
    OPTIONAL_EXT(has_eds2, VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME, eds2, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT);
    OPTIONAL_EXT(has_eds3, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME, eds3, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT);
    OPTIONAL_EXT(has_vertex_input_dynamic_state, VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME, vids, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT);
    OPTIONAL_EXT(has_conditional_rendering, VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME, cond, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT);
    OPTIONAL_EXT(has_transform_feedback, VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME, xfb, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT);
    OPTIONAL_EXT(has_provoking_vertex, VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME, pv, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT);
    OPTIONAL_EXT(has_robustness2, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME, rob2, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT);
    OPTIONAL_EXT(has_device_fault, VK_EXT_DEVICE_FAULT_EXTENSION_NAME, fault, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT);
    // Task and mesh programs; without it their draws are skipped.
    OPTIONAL_EXT(has_mesh, VK_EXT_MESH_SHADER_EXTENSION_NAME, mesh, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT);
    // A pipeline built for fewer colour targets than its pass has (depth-only draws in MKW's 6-target G-buffer pass:
    // run99 validation, VUID-vkCmdDraw-dynamicRenderingUnusedAttachments-08910).
    OPTIONAL_EXT(has_unused_attachments, VK_EXT_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_EXTENSION_NAME, unused,
                 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_FEATURES_EXT);
    // Ryujinx's SPIR-V declares fragment shader interlock whenever the host reports it (vk_pipeline.c spirv_fixup).
    OPTIONAL_EXT(has_fragment_shader_interlock, VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME, fsi,
                 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT);
    // Feedback loops (vk.h VkCtx.feedback): the layout, and barriers inside dynamic rendering
    int has_fbl = 0, has_lr = 0;
    if (has_ext(ep, ne, VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME) &&
        has_ext(ep, ne, VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME)) {
        has_fbl = has_lr = 1;
        ext[n_ext++] = VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME;
        ext[n_ext++] = VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME;
        CHAIN(fbl, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT);
        CHAIN(lr, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR);
    }
    // Placed maps (vk_memory.c placed_pool): MKW_VK_PLACED=1 only, an experiment. The write watch (userfaultfd,
    // PAGEMAP_SCAN) does not see writes into the driver's mapping, so every page there counts as written and its
    // textures would be uploaded again at every check.
    int has_placed = 0;
    if (has_ext(ep, ne, VK_EXT_MAP_MEMORY_PLACED_EXTENSION_NAME) && has_ext(ep, ne, VK_KHR_MAP_MEMORY_2_EXTENSION_NAME) &&
        cfg_get("MKW_VK_PLACED") && atoi(cfg_get("MKW_VK_PLACED"))) {
        has_placed = 1;
        ext[n_ext++] = VK_EXT_MAP_MEMORY_PLACED_EXTENSION_NAME;
        ext[n_ext++] = VK_KHR_MAP_MEMORY_2_EXTENSION_NAME;
        CHAIN(placed, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT);
    }
    if (has_ext(ep, ne, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME)) {
        g_vk.has_emh = 1;
        ext[n_ext++] = VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME;
    } else {
        rt_log("vk: no VK_EXT_external_memory_host: a pool whose shadow does not fit cannot be imported");
    }
    if (has_ext(ep, ne, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME)) { g_vk.has_memory_budget = 1; ext[n_ext++] = VK_EXT_MEMORY_BUDGET_EXTENSION_NAME; }
    if (g_vk.has_colorspace && has_ext(ep, ne, VK_EXT_HDR_METADATA_EXTENSION_NAME)) {
        g_vk.has_hdr_metadata = 1;
        ext[n_ext++] = VK_EXT_HDR_METADATA_EXTENSION_NAME;
    }
    if (has_ext(ep, ne, VK_AMD_BUFFER_MARKER_EXTENSION_NAME)) { g_vk.has_buffer_marker = 1; ext[n_ext++] = VK_AMD_BUFFER_MARKER_EXTENSION_NAME; }
    // The breadcrumbs' fallback where the buffer marker is missing (NVIDIA's drivers have both: GTX 1050 Ti on Windows,
    // RTX 5070 Ti 620.70 on Linux, 09/11/2026); only with the breadcrumbs on: enabling it may cost even unused.
    if (!g_vk.has_buffer_marker && has_ext(ep, ne, VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME) &&
        rt_knob("GPU_CRUMBS", "1")) {
        g_vk.has_nv_checkpoints = 1;
        ext[n_ext++] = VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME;
    }
    if (has_ext(ep, ne, VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME) && vk_tl_on()) {   // vk_timeline.c
        g_vk.has_calibrated = 1;
        ext[n_ext++] = VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME;
    }
    // MKW_SPEC_STATS=1 (vk_pipeline.c): the driver's statistics of each optimised graphics pipeline. A measurement only.
    if (has_ext(ep, ne, VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME) && cfg_get("MKW_SPEC_STATS") &&
        atoi(cfg_get("MKW_SPEC_STATS"))) {
        g_vk.has_exec_stats = 1;
        ext[n_ext++] = VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME;
        CHAIN(pexe, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR);
    }
    // Graphics pipeline library: chained last so it can be dropped again when vk_caps_gpl declines it.
    void **gpl_link = NULL;
    if (has_ext(ep, ne, VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME) && has_ext(ep, ne, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME)) {
        ext[n_ext++] = VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME;
        ext[n_ext++] = VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME;
        gpl_link = tail;
        CHAIN(gpl, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT);
    }
    // MKW_LATENCY only (vk_latency.c): when each picture reached the screen
    int want_pw = vk_lat_on() && has_ext(ep, ne, VK_KHR_PRESENT_ID_EXTENSION_NAME) &&
                  has_ext(ep, ne, VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
    if (want_pw) {
        ext[n_ext++] = VK_KHR_PRESENT_ID_EXTENSION_NAME;
        ext[n_ext++] = VK_KHR_PRESENT_WAIT_EXTENSION_NAME;
        CHAIN(pid, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR);
        CHAIN(pwait, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR);
    }
    free(ep);

    // The driver fills the chain with what it supports; the same chain then enables all of it (robustBufferAccess
    // stays on: guest shaders index constant buffers freely and out-of-range reads must stay harmless).
    vkGetPhysicalDeviceFeatures2(g_vk.phys, &c->f2);
    vk_caps_features(&c->f2.features);
    int has_pw = want_pw && c->pid.presentId && c->pwait.presentWait;
    if (gpl_link) {
        VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT gp = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &gp};
        vkGetPhysicalDeviceProperties2(g_vk.phys, &p2);
        if (!vk_caps_gpl(&c->gpl, &gp)) { *gpl_link = NULL; n_ext -= 2; }
    }
    if (g_vk.has_mesh) {
        // Only the two stages: the other mesh features need multiview or fragment shading rate enabled with them.
        c->mesh.multiviewMeshShader = c->mesh.primitiveFragmentShadingRateMeshShader = c->mesh.meshShaderQueries = VK_FALSE;
        g_vk.has_mesh = c->mesh.taskShader && c->mesh.meshShader;
        rt_log("vk: mesh shaders: %s", g_vk.has_mesh ? "task and mesh" : "not supported, task/mesh draws are skipped");
    }
    const VkPhysicalDeviceFeatures *h = &c->f2.features;
    const char *miss = NULL;
    if (!c->f12.timelineSemaphore) miss = "timelineSemaphore";
    else if (!c->f12.drawIndirectCount) miss = "drawIndirectCount";   // MultiDrawElementsIndirectCount
    else if (!c->f13.dynamicRendering) miss = "dynamicRendering";
    else if (!c->f13.synchronization2) miss = "synchronization2";
    else if (!c->f13.shaderDemoteToHelperInvocation) miss = "shaderDemoteToHelperInvocation";   // KILL (sm86 core.py)
    else if (!h->shaderStorageImageWriteWithoutFormat) miss = "shaderStorageImageWriteWithoutFormat";
    else if (!c->clip.depthClipControl) miss = "depthClipControl";
    else if (!h->shaderInt64) miss = "shaderInt64";
    else if (!h->samplerAnisotropy) miss = "samplerAnisotropy";
    else if (!h->independentBlend) miss = "independentBlend";
    else if (!h->geometryShader) miss = "geometryShader";
    else if (!h->textureCompressionBC) miss = "textureCompressionBC";
    if (miss) { rt_log("vk: required feature missing on %s: %s", g_vk.props.deviceName, miss); free(c); return 0; }
    g_vk.has_buffer_device_address = c->f12.bufferDeviceAddress;
    g_vk.has_placed = has_placed && c->placed.memoryMapPlaced && c->placed.memoryUnmapReserve;
    c->placed.memoryMapRangePlaced = VK_FALSE;   // whole allocations only
    g_vk.has_astc_ldr = h->textureCompressionASTC_LDR;
    {
        const char *e = cfg_get("MKW_FEEDBACK");
        int layout_ok = has_fbl && has_lr && c->fbl.attachmentFeedbackLoopLayout && c->lr.dynamicRenderingLocalRead;
        g_vk.feedback = e && !strcmp(e, "0") ? VK_FEEDBACK_OFF
                      : layout_ok && !(e && !strcmp(e, "restart")) ? VK_FEEDBACK_LAYOUT : VK_FEEDBACK_RESTART;
        rt_log("vk: feedback loops: %s (attachmentFeedbackLoopLayout %d, dynamicRenderingLocalRead %d)",
               g_vk.feedback == VK_FEEDBACK_LAYOUT ? "feedback layout + by-region barrier"
               : g_vk.feedback == VK_FEEDBACK_RESTART ? "pass restart + full barrier" : "off (MKW_FEEDBACK=0)",
               has_fbl && c->fbl.attachmentFeedbackLoopLayout, has_lr && c->lr.dynamicRenderingLocalRead);
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi[2] = {
        {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = g_vk.gfx_family, .queueCount = 1,
         .pQueuePriorities = &prio}};
    uint32_t nqi = 1;
    if (g_vk.xfer_family != g_vk.gfx_family) {
        qi[1] = (VkDeviceQueueCreateInfo){.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                          .queueFamilyIndex = g_vk.xfer_family, .queueCount = 1, .pQueuePriorities = &prio};
        nqi = 2;
    }
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &c->f2, .queueCreateInfoCount = nqi,
                             .pQueueCreateInfos = qi, .enabledExtensionCount = n_ext, .ppEnabledExtensionNames = ext};
    vk_guard_gpu_ok();
    VkResult r = vkCreateDevice(g_vk.phys, &di, NULL, &g_vk.device);
    free(c);
    if (r != VK_SUCCESS) { rt_log("vk: vkCreateDevice failed (%d)", (int)r); return 0; }
    vk_lat_device(g_vk.device, has_pw);
    vkGetDeviceQueue(g_vk.device, g_vk.gfx_family, 0, &g_vk.gfx_queue);
    if (g_vk.xfer_family != g_vk.gfx_family) vkGetDeviceQueue(g_vk.device, g_vk.xfer_family, 0, &g_vk.xfer_queue);
    else { g_vk.xfer_queue = g_vk.gfx_queue; g_vk.xfer_shared = 1; }

    VkPhysicalDeviceExternalMemoryHostPropertiesEXT emh = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &emh};
    if (g_vk.has_emh) {
        vkGetPhysicalDeviceProperties2(g_vk.phys, &p2);
        g_vk.min_imported_host_alignment = (uint32_t)emh.minImportedHostPointerAlignment;
    }
    vk_caps_init(&g_vk);

    VmaAllocatorCreateInfo ai = {.physicalDevice = g_vk.phys, .device = g_vk.device, .instance = g_vk.instance,
                                 .vulkanApiVersion = VK_API_VERSION_1_3,
                                 .flags = (g_vk.has_buffer_device_address ? VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT : 0) |
                                          (g_vk.has_memory_budget ? VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT : 0),
                                 .pDeviceMemoryCallbacks = vk_headroom_callbacks()};
    static const VmaVulkanFunctions vf = {.vkAllocateMemory = timed_alloc};   // the rest from VMA's static imports
    ai.pVulkanFunctions = &vf;
    // MKW_GPU_MEM_LIMIT=<MiB>: a cap on what VMA allocates, split over the heaps by size. On an integrated GPU this
    // memory is system RAM outside the process's cgroup (run47: the global OOM killer took other applications), so
    // unset there it is 4096, what every scripted run sets (3072 makes the race black), or more with more RAM, plus the
    // GPU's firmware carve-out (vk_default_mem_limit_mib); a player
    // starting the game from its folder has no script to set it. 0 lifts it. On a discrete GPU it caps the VRAM heaps
    // alone: split with the host heap too, a player's 6400 left an RTX 2070 SUPER 1 040 MiB of its 8 GB (#28).
    VkDeviceSize limits[VK_MAX_MEMORY_HEAPS];
    VkPhysicalDeviceProperties dp;
    vkGetPhysicalDeviceProperties(g_vk.phys, &dp);
    const char *lim = cfg_get("MKW_GPU_MEM_LIMIT");
    char lim_ram[24];
    if (!lim && dp.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
        float used, total = 0, proc;
        plat_mem_info(&used, &total, &proc);
        VkPhysicalDeviceIDProperties id = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &id};
        vkGetPhysicalDeviceProperties2(g_vk.phys, &p2);
        uint64_t carve = plat_gpu_vram_mib(dp.vendorID, dp.deviceID, id.deviceLUIDValid ? id.deviceLUID : NULL);
        snprintf(lim_ram, sizeof lim_ram, "%llu",
                 (unsigned long long)vk_default_mem_limit_mib(total > 0 ? (uint64_t)total : 0, carve));
        rt_log("vk: integrated GPU, %llu MiB carved out of RAM by the firmware: memory cap %s MiB", (unsigned long long)carve,
               lim_ram);
        lim = lim_ram;
    }
    if (lim && atoll(lim) > 0) {
        VkDeviceSize total = 0, cap = (VkDeviceSize)atoll(lim) << 20;
        int discrete = dp.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
        uint32_t counted = 0;   // the heaps the cap is split over
        for (uint32_t h = 0; h < g_vk.mem_props.memoryHeapCount; h++) {
            const VkMemoryHeap *mh = &g_vk.mem_props.memoryHeaps[h];
            if (!discrete || (mh->flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT && mh->size >= (1ull << 30)))
                counted |= 1u << h, total += mh->size;
        }
        for (uint32_t h = 0; h < g_vk.mem_props.memoryHeapCount; h++) {
            VkDeviceSize size = g_vk.mem_props.memoryHeaps[h].size;
            limits[h] = (counted >> h & 1) && total > cap ? (VkDeviceSize)((double)cap * (double)size / (double)total) : size;
            // MKW_GPU_MEM_LIMIT_HEAP<n>=<MiB> (diagnosis): one heap's cap alone. The split keeps the heaps' ratio, so a
            // low limit fills them all at once; cap-race1 and cap-race6 died with heap0 at its cap and heap1 with room.
            char name[32];
            snprintf(name, sizeof name, "MKW_GPU_MEM_LIMIT_HEAP%u", h);
            const char *hl = cfg_get(name);
            if (hl && atoll(hl) > 0) limits[h] = (VkDeviceSize)atoll(hl) << 20;
            rt_log("vk: heap %u limited to %llu MiB (MKW_GPU_MEM_LIMIT)", h, (unsigned long long)(limits[h] >> 20));
        }
        ai.pHeapSizeLimit = limits;
    } else if (!lim && dp.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU && g_vk.has_memory_budget) {
        // A discrete GPU with MKW_GPU_MEM_LIMIT unset: each VRAM heap stops MKW_VRAM_MARGIN (MiB) below the driver's
        // budget at start. Past the budget the GTX 1050 Ti's driver moved some of it to system memory and the race's
        // GPU went 29-32 -> 42-49 ms in 6 of 15 runs; heap 0 capped below it, 0 of 4 (wtop1-4).
        VkPhysicalDeviceMemoryBudgetPropertiesEXT b = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
        VkPhysicalDeviceMemoryProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, .pNext = &b};
        vkGetPhysicalDeviceMemoryProperties2(g_vk.phys, &p2);
        const char *mg = cfg_get("MKW_VRAM_MARGIN");
        VkDeviceSize margin = (VkDeviceSize)(mg ? atoll(mg) : 300) << 20;
        int any = 0;
        for (uint32_t h = 0; h < g_vk.mem_props.memoryHeapCount; h++) {
            VkDeviceSize size = g_vk.mem_props.memoryHeaps[h].size;
            limits[h] = size;
            // The small device-local heap of a resizable BAR window (214 MiB on the 1050 Ti) keeps its size
            if (!(g_vk.mem_props.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) || size < (1ull << 30)) continue;
            if (b.heapBudget[h] > margin && b.heapBudget[h] - margin < size) limits[h] = b.heapBudget[h] - margin, any = 1;
            rt_log("vk: heap %u limited to %llu MiB: budget %llu MiB less MKW_VRAM_MARGIN (MKW_GPU_MEM_LIMIT=0 lifts it)", h,
                   (unsigned long long)(limits[h] >> 20), (unsigned long long)(b.heapBudget[h] >> 20));
        }
        if (any) ai.pHeapSizeLimit = limits;
    }
    if ((r = vmaCreateAllocator(&ai, &g_vk.vma)) != VK_SUCCESS) { rt_log("vk: vmaCreateAllocator failed (%d)", (int)r); return 0; }

    VkSemaphoreTypeCreateInfo st = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                    .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = 0};
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &st};
    if ((r = vkCreateSemaphore(g_vk.device, &si, NULL, &g_vk.timeline)) != VK_SUCCESS) {
        rt_log("vk: timeline semaphore creation failed (%d)", (int)r);
        return 0;
    }
    return 1;
}

volatile uint64_t vk_destroy_gen;   // vk_destroy_count.h

static long g_errors_total;     // outlives the device so the probe can print it after vk_shutdown

static void destroy_all(void) {
    gfx_window_release_vk();
    if (g_vk.device) vkDeviceWaitIdle(g_vk.device);
    if (g_vk.device) vk_astc_release(), vk_crumbs_release();
    if (g_vk.timeline) vkDestroySemaphore(g_vk.device, g_vk.timeline, NULL);
    vk_headroom_stop();
    vk_tex_pool_stop();
    vk_sparse_shutdown();
    if (g_vk.vma) vmaDestroyAllocator(g_vk.vma);
    if (g_vk.device) vkDestroyDevice(g_vk.device, NULL);
    if (g_vk.messenger) {
        PFN_vkDestroyDebugUtilsMessengerEXT f =
            (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_vk.instance, "vkDestroyDebugUtilsMessengerEXT");
        if (f) f(g_vk.instance, g_vk.messenger, NULL);
    }
    if (g_vk.instance) vkDestroyInstance(g_vk.instance, NULL);
    g_errors_total += g_vk.validation_errors;
    memset(&g_vk, 0, sizeof g_vk);
}

long vk_validation_errors_total(void) { return g_errors_total + (g_ready ? g_vk.validation_errors : 0); }

int vk_init(void) {
    plat_mutex_lock(&g_lock);
    if (!g_ready) {
        memset(&g_vk, 0, sizeof g_vk);
        if (create_instance() && pick_device() && create_device())
            g_ready = 1, vk_crumbs_init(), vk_headroom_start(), vk_tex_pool_start();
        else destroy_all();
    }
    int ok = g_ready;
    plat_mutex_unlock(&g_lock);
    return ok;
}

void vk_shutdown(void) {
    plat_mutex_lock(&g_lock);
    if (g_ready) { destroy_all(); g_ready = 0; }
    plat_mutex_unlock(&g_lock);
}

VkCtx *vk_ctx(void) { return g_ready ? &g_vk : NULL; }

int vk_selftest(void) {
    if (!vk_init()) return 1;
    VkCtx *c = &g_vk;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 64 << 10,
                             .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    VmaAllocationCreateInfo aci = {.usage = VMA_MEMORY_USAGE_AUTO, .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT};
    VkBuffer buf;
    VmaAllocation alloc;
    VkResult r = vmaCreateBuffer(c->vma, &bi, &aci, &buf, &alloc, NULL);
    if (r != VK_SUCCESS) { rt_log("vk selftest: vmaCreateBuffer failed (%d)", (int)r); return 2; }
    vmaDestroyBuffer(c->vma, buf, alloc);

    VkSemaphoreSignalInfo sig = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, .semaphore = c->timeline, .value = 1};
    if ((r = vkSignalSemaphore(c->device, &sig)) != VK_SUCCESS) { rt_log("vk selftest: vkSignalSemaphore failed (%d)", (int)r); return 3; }
    uint64_t value = 1;
    VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &c->timeline,
                              .pValues = &value};
    if ((r = vkWaitSemaphores(c->device, &wi, 1000000000ull)) != VK_SUCCESS) { rt_log("vk selftest: vkWaitSemaphores failed (%d)", (int)r); return 4; }
    uint64_t now = 0;
    vkGetSemaphoreCounterValue(c->device, c->timeline, &now);
    if (now != 1) { rt_log("vk selftest: timeline value %llu, expected 1", (unsigned long long)now); return 5; }
    return 0;
}

// --probe-vkdevice (host.h)
int probe_vkdevice(void) {
    int rc = vk_selftest();
    VkCtx *c = vk_ctx();
    if (rc || !c) { printf("probe-vkdevice: FAILED (selftest %d)\n", rc ? rc : 1); vk_shutdown(); return 1; }
    uint32_t v = c->props.apiVersion;
    printf("probe-vkdevice: device %d \"%s\" api %u.%u.%u, gfx queue family %u, transfer family %u%s, host import alignment %#x\n",
           c->device_index, c->props.deviceName, VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v),
           c->gfx_family, c->xfer_family, c->xfer_shared ? " (shared queue)" : "", c->min_imported_host_alignment);
    printf("probe-vkdevice: optional: custom_border=%d eds=%d eds2=%d eds3=%d vertex_input=%d cond_render=%d xfb=%d provoking=%d "
           "robustness2=%d fault=%d budget=%d bda=%d interlock=%d validation=%d\n",
           c->has_custom_border_color, c->has_eds, c->has_eds2, c->has_eds3, c->has_vertex_input_dynamic_state,
           c->has_conditional_rendering, c->has_transform_feedback, c->has_provoking_vertex, c->has_robustness2,
           c->has_device_fault, c->has_memory_budget, c->has_buffer_device_address, c->has_fragment_shader_interlock,
           c->validation);
    vk_shutdown();
    printf("probe-vkdevice: %s (validation errors %ld)\n", g_errors_total ? "FAILED" : "passed", g_errors_total);
    return g_errors_total ? 1 : 0;
}
