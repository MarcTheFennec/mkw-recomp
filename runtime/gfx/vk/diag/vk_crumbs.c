// GPU breadcrumbs for a device loss (vk.h vk_crumbs_*). VK_AMD_buffer_marker has the command stream write a 32-bit
// value into host memory: at TOP_OF_PIPE once the command processor reaches it, at BOTTOM_OF_PIPE once every command
// before it has finished. Each mark writes its start at the top and its end at the bottom into two words of one
// host-coherent buffer, so after a loss one word names the newest mark the GPU started and the other the newest it
// had finished: the hang lies between them. Words 0 and 1: upload batches (vk_mem_upload.c); words 2 and 3: the
// replay's render passes and dispatches (vk_exec.c, vk_exec_ops.c). A ring of records says what each mark was.
//
// Why: issues #4 and #5 (AMD discrete GPUs, v0.1.2) logged only "upload submit failed (-4)": the queue the loss was
// noticed on, not the work that hung. VK_EXT_device_fault (vk_exec_dstat.c) names an address only for a page fault;
// these marks also cover a timeout. Cost: two small writes per pass or dispatch, none per draw; a barrier already
// precedes each pass and dispatch, so the end-of-pipe write adds no wait. MKW_GPU_CRUMBS=0 turns them off.
// Where the driver has no buffer marker but VK_NV_device_diagnostic_checkpoints (older NVIDIA drivers; current ones
// have both), the same marks are checkpoints, read from the queue after the loss.
// The design follows totk-pc-recomp's vk_crumbs.c (MIT, 710b7f5) and Savage Wind Recomp's GPU checkpoints; the code
// is ours.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "nvn_api.h"
#include "platform/platform.h"
#include "vk.h"
#include "vk_rec.h"

#define CRUMB_MAGIC 0x4d4b5743u   // 'MKWC'

typedef struct {
    uint32_t magic;
    uint32_t active;
    uint64_t frame;
    uint64_t pipe_hash;
    uint8_t shader_a[32];
    uint8_t shader_b[32];
    uint8_t has_shader_a;
    uint8_t has_shader_b;
    uint8_t in_mesh_compute;
    uint8_t pad[5];
    char pass[128];
    char command[128];
} VkLastCrumb;

static VkLastCrumb g_crumb_mem;
static VkLastCrumb *g_crumb = &g_crumb_mem;
static VkLastCrumb *g_crumb_alt;
static int g_crumb_mapped;

static void ensure_crumb_map(void) {
    if (__atomic_load_n(&g_crumb_mapped, __ATOMIC_ACQUIRE)) return;
    VkLastCrumb *m1 = NULL, *m2 = NULL;
    const char *home = getenv("HOME");
    if (home && *home) {
        char path[1024];
        if (snprintf(path, sizeof path, "%s/.local/share/mkw-recomp/logs/last_crumb.bin", home) < (int)sizeof path)
            m1 = plat_mmap_rw_file(path, 4096);
    }
    char exe[PLAT_PATH_MAX * 3];
    if (plat_exe_path(exe, sizeof exe)) {
        char *cut = plat_last_sep(exe);
        char *f = strrchr(exe, '/');
        if (f > cut) cut = f;
        if (cut && (size_t)(cut - exe) + sizeof "/logs/last_crumb.bin" <= sizeof exe) {
            strcpy(cut, "/logs/last_crumb.bin");
            m2 = plat_mmap_rw_file(exe, 4096);
        }
    }
    if (m1) {
        g_crumb = m1;
        g_crumb_alt = m2;
    } else if (m2) {
        g_crumb = m2;
    }
    __atomic_store_n(&g_crumb_mapped, 1, __ATOMIC_RELEASE);
}

static void sync_alt_crumb(void) {
    if (g_crumb_alt && g_crumb != g_crumb_alt)
        memcpy(g_crumb_alt, g_crumb, sizeof *g_crumb);
}

static void format_shaders(const VkLastCrumb *c, char *out, size_t size) {
    if (!c->has_shader_a && !c->has_shader_b) {
        snprintf(out, size, "none");
        return;
    }
    char a[65] = "", b[65] = "";
    if (c->has_shader_a)
        for (int k = 0; k < 32; k++) sprintf(a + 2 * k, "%02x", c->shader_a[k]);
    if (c->has_shader_b)
        for (int k = 0; k < 32; k++) sprintf(b + 2 * k, "%02x", c->shader_b[k]);
    if (c->has_shader_a && c->has_shader_b)
        snprintf(out, size, "%s+%s", a, b);
    else
        snprintf(out, size, "%s", c->has_shader_a ? a : b);
}

void vk_crumbs_last_check(void) {
    ensure_crumb_map();
    VkLastCrumb *c = g_crumb;
    if (c->magic != CRUMB_MAGIC || !c->active) {
        if (g_crumb_alt && g_crumb_alt->magic == CRUMB_MAGIC && g_crumb_alt->active) c = g_crumb_alt;
        else return;
    }
    char sh[140];
    format_shaders(c, sh, sizeof sh);
    rt_log("last run crashed at: %s / shader %s (pipeline %#018llx) / %s%s (frame %llu)",
           c->pass[0] ? c->pass : "no pass", sh, (unsigned long long)c->pipe_hash,
           c->command[0] ? c->command : "unknown",
           c->in_mesh_compute ? " [MKW_MESH_COMPUTE]" : "",
           (unsigned long long)c->frame);
    memset(g_crumb, 0, sizeof *g_crumb);
    g_crumb->magic = CRUMB_MAGIC;
    if (g_crumb_alt) {
        memset(g_crumb_alt, 0, sizeof *g_crumb_alt);
        g_crumb_alt->magic = CRUMB_MAGIC;
    }
}

void vk_crumbs_set_pass(uint32_t w, uint32_t h, uint32_t d, uint32_t ncolor, const VkFormat *color, VkFormat depth) {
    ensure_crumb_map();
    VkLastCrumb *c = g_crumb;
    c->magic = CRUMB_MAGIC;
    c->frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    size_t n = (size_t)snprintf(c->pass, sizeof c->pass, "pass %ux%u, %u layers, color", w, h, d);
    for (uint32_t i = 0; i < ncolor && i < 8 && n < sizeof c->pass; i++)
        n += (size_t)snprintf(c->pass + n, sizeof c->pass - n, "%s%d", i ? "," : " ", color ? color[i] : 0);
    if (!ncolor && n < sizeof c->pass) n += (size_t)snprintf(c->pass + n, sizeof c->pass - n, " none");
    if (n < sizeof c->pass) snprintf(c->pass + n, sizeof c->pass - n, ", depth %d", depth);
    __atomic_store_n(&c->active, 1u, __ATOMIC_RELEASE);
    sync_alt_crumb();
}

void vk_crumbs_set_state(uint64_t pipe_hash, const uint8_t *sa, const uint8_t *sb, const char *cmd, int in_mesh_compute) {
    ensure_crumb_map();
    VkLastCrumb *c = g_crumb;
    c->magic = CRUMB_MAGIC;
    c->frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    if (pipe_hash) c->pipe_hash = pipe_hash;
    if (sa) {
        memcpy(c->shader_a, sa, 32);
        c->has_shader_a = 1;
    }
    if (sb) {
        memcpy(c->shader_b, sb, 32);
        c->has_shader_b = 1;
    } else if (sa) {
        c->has_shader_b = 0;
    }
    c->in_mesh_compute = (uint8_t)(in_mesh_compute != 0);
    if (cmd) snprintf(c->command, sizeof c->command, "%s", cmd);
    __atomic_store_n(&c->active, 1u, __ATOMIC_RELEASE);
    sync_alt_crumb();
}

void vk_crumbs_set_mesh_compute(int in_mesh_compute) {
    ensure_crumb_map();
    g_crumb->in_mesh_compute = (uint8_t)(in_mesh_compute != 0);
    sync_alt_crumb();
}

void vk_crumbs_dump_active(void) {
    ensure_crumb_map();
    const VkLastCrumb *c = g_crumb;
    if (c->magic != CRUMB_MAGIC) return;
    char sh[140];
    format_shaders(c, sh, sizeof sh);
    rt_log("vk: active GPU breadcrumb: pass=[%s] pipeline=%#018llx shader=%s command=[%s] in_mesh_compute=%u frame=%llu",
           c->pass[0] ? c->pass : "none", (unsigned long long)c->pipe_hash, sh,
           c->command[0] ? c->command : "none", (unsigned)c->in_mesh_compute, (unsigned long long)c->frame);
}

void vk_crumbs_clear_active(void) {
    ensure_crumb_map();
    __atomic_store_n(&g_crumb->active, 0u, __ATOMIC_RELEASE);
    if (g_crumb_alt) __atomic_store_n(&g_crumb_alt->active, 0u, __ATOMIC_RELEASE);
}

enum { RING = 4096 };   // records kept: a loss is noticed within a few frames of the hang
typedef struct {
    uint32_t id;
    uint8_t kind, ncolor;
    uint64_t frame;
    uint32_t w, h, d;   // pass: render area and layers; dispatch: groups (0 indirect); upload: w its batch number
    VkFormat color[8], depth;
} Mark;

static PFN_vkCmdWriteBufferMarkerAMD write_marker;
static PFN_vkCmdSetCheckpointNV set_checkpoint;   // NVIDIA: a checkpoint (id << 1 | end) per mark instead of a write
static int active;                                // marks are recorded (either way)
static VkBuffer buf;
static VmaAllocation alloc;
static volatile uint32_t *words;
static Mark ring[RING];
static uint32_t seq;   // marks made; several replay threads (MKW_REPLAY_PAR) take ids at once
static int mode;       // rt_knob: 1 on, 2 "ab" (on in the B blocks, for its A/B)

void vk_crumbs_init(void) {
    VkCtx *c = vk_ctx();
    mode = rt_knob("GPU_CRUMBS", "1");
    if (!c || !mode) return;
    if (c->has_nv_checkpoints) {
        set_checkpoint = (PFN_vkCmdSetCheckpointNV)vkGetDeviceProcAddr(c->device, "vkCmdSetCheckpointNV");
        active = set_checkpoint != NULL;
        rt_log("vk: GPU breadcrumbs %s%s: upload batches, render passes and dispatches as NVIDIA checkpoints "
               "(MKW_GPU_CRUMBS=0 turns them off)", active ? "on" : "unavailable",
               active && mode == 2 ? " in the B blocks (MKW_AB)" : "");
        return;
    }
    if (!c->has_buffer_marker) {
        rt_log("vk: GPU breadcrumbs unavailable (no VK_AMD_buffer_marker or VK_NV_device_diagnostic_checkpoints)");
        return;
    }
    write_marker = (PFN_vkCmdWriteBufferMarkerAMD)vkGetDeviceProcAddr(c->device, "vkCmdWriteBufferMarkerAMD");
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 16,
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                  .requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
    VmaAllocationInfo info;
    if (!write_marker || vmaCreateBuffer(c->vma, &bi, &ai, &buf, &alloc, &info) != VK_SUCCESS) {
        rt_log("vk: GPU breadcrumbs unavailable (no marker buffer)");
        write_marker = NULL;
        return;
    }
    words = info.pMappedData;
    words[0] = words[1] = words[2] = words[3] = 0;
    active = 1;
    rt_log("vk: GPU breadcrumbs on%s: upload batches, render passes and dispatches (MKW_GPU_CRUMBS=0 turns them off)",
           mode == 2 ? " in the B blocks (MKW_AB)" : "");
}

void vk_crumbs_release(void) {
    vk_crumbs_clear_active();
    VkCtx *c = vk_ctx();
    if (buf && c) vmaDestroyBuffer(c->vma, buf, alloc);
    buf = VK_NULL_HANDLE, alloc = NULL, words = NULL, write_marker = NULL, set_checkpoint = NULL, active = 0;
}

static void mark(VkCommandBuffer cb, uint32_t word, uint32_t id) {
    if (set_checkpoint) rec_checkpoint(set_checkpoint, cb, (const void *)(uintptr_t)((uint64_t)id << 1 | (word & 1)));
    else rec_marker(write_marker, cb, word & 1 ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    buf, 4 * word, id);
}

uint32_t vk_crumb_begin(VkCommandBuffer cb, int kind, uint32_t w, uint32_t h, uint32_t d, uint32_t ncolor,
                        const VkFormat *color, VkFormat depth) {
    if (kind == VK_CRUMB_PASS) vk_crumbs_set_pass(w, h, d, ncolor, color, depth);
    if (!active || !cb || !RT_KNOB_ON(mode)) return 0;
    uint32_t id = __atomic_add_fetch(&seq, 1, __ATOMIC_RELAXED);
    if (!id) id = __atomic_add_fetch(&seq, 1, __ATOMIC_RELAXED);   // 0 means no mark
    Mark *m = &ring[id % RING];
    *m = (Mark){.id = id, .kind = (uint8_t)kind, .ncolor = (uint8_t)(ncolor > 8 ? 8 : ncolor), .w = w, .h = h, .d = d,
                .depth = depth, .frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED)};
    if (color) memcpy(m->color, color, sizeof *color * m->ncolor);
    mark(cb, kind == VK_CRUMB_UPLOAD ? 0 : 2, id);
    return id;
}

void vk_crumb_end(VkCommandBuffer cb, int kind, uint32_t id) {
    if (active && cb && id) mark(cb, kind == VK_CRUMB_UPLOAD ? 1 : 3, id);
}

static void describe(uint32_t id, char *o, size_t size) {
    const Mark *m = &ring[id % RING];
    if (!id) {
        snprintf(o, size, "nothing yet");
        return;
    }
    if (m->id != id) {
        snprintf(o, size, "%u (no record: a later mark took its slot)", id);
        return;
    }
    if (m->kind == VK_CRUMB_UPLOAD) {
        snprintf(o, size, "%u, upload batch %u in frame %llu", id, m->w, (unsigned long long)m->frame);
    } else if (m->kind == VK_CRUMB_DISPATCH) {
        snprintf(o, size, "%u, dispatch in frame %llu: %ux%ux%u groups%s", id, (unsigned long long)m->frame, m->w, m->h,
                 m->d, m->w ? "" : " (indirect)");
    } else {
        size_t n = (size_t)snprintf(o, size, "%u, render pass in frame %llu: %ux%u, %u layers, colour formats", id,
                                    (unsigned long long)m->frame, m->w, m->h, m->d);
        for (int i = 0; i < m->ncolor && n < size; i++) n += (size_t)snprintf(o + n, size - n, "%s%d", i ? "," : " ", m->color[i]);
        if (!m->ncolor && n < size) n += (size_t)snprintf(o + n, size - n, " none");
        if (n < size) snprintf(o + n, size - n, ", depth format %d", m->depth);
    }
}

// NVIDIA: the last checkpoint each pipeline stage of the graphics queue (uploads and the replay both) executed. A
// checkpoint is a mark's id times 2, plus 1 at its end: the top-of-pipe one is the newest point the GPU reached, the
// bottom-of-pipe one the newest before which everything had finished.
static void nv_report(void) {
    VkCtx *c = vk_ctx();
    PFN_vkGetQueueCheckpointDataNV get =
        c ? (PFN_vkGetQueueCheckpointDataNV)vkGetDeviceProcAddr(c->device, "vkGetQueueCheckpointDataNV") : NULL;
    if (!get) return;
    VkCheckpointDataNV d[32];
    uint32_t n = 0;
    get(c->gfx_queue, &n, NULL);
    if (n > 32) n = 32;
    for (uint32_t i = 0; i < n; i++) d[i] = (VkCheckpointDataNV){.sType = VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV};
    get(c->gfx_queue, &n, d);
    rt_log("vk: GPU breadcrumbs (%u marks made, NVIDIA checkpoints): %u stages reported", __atomic_load_n(&seq, __ATOMIC_RELAXED),
           n);
    for (uint32_t i = 0; i < n; i++) {
        uint64_t v = (uint64_t)(uintptr_t)d[i].pCheckpointMarker;
        char a[256];
        describe((uint32_t)(v >> 1), a, sizeof a);
        rt_log("vk:   stage %#x last ran the %s of mark %s", (unsigned)d[i].stage, v & 1 ? "end" : "start", a);
    }
}

void vk_crumbs_report(void) {
    static int done;
    if (!active || __atomic_exchange_n(&done, 1, __ATOMIC_ACQ_REL)) return;
    if (set_checkpoint) {
        nv_report();
        return;
    }
    static const char *const what[2] = {"upload batches", "replay passes and dispatches"};
    rt_log("vk: GPU breadcrumbs (%u marks made): the hang lies after the last finished mark and up to the last started",
           __atomic_load_n(&seq, __ATOMIC_RELAXED));
    for (int k = 0; k < 2; k++) {
        char a[256], b[256];
        describe(words[2 * k], a, sizeof a);
        describe(words[2 * k + 1], b, sizeof b);
        rt_log("vk:   %s: last started %s", what[k], a);
        rt_log("vk:   %s: last finished %s", what[k], b);
    }
}
