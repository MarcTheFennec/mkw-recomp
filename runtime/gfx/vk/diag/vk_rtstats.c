// MKW_RTSTATS=<N> (diagnosis, off by default): one frame in N (60 when the value is not a number above 0), every
// render pass's colour and depth targets and every storage image a dispatch writes are copied into host buffers after
// the pass, and threads of their own read their values once the submission is done. A target is reported in the log
// ("rtstats:" lines) when
//   nan       some texel holds a NaN (with how many and their bounding box: a row or a column is a clue)
//   inf       some texel holds an infinity
//   constant  every texel holds the same value (16x16 and up): a pass that wrote nothing, or wrote one value
//             everywhere (an ambient occlusion target all at 1 renders as a flat image and logs nothing else)
// Each report goes out once per rule, target slot, format, size and program (ex->prog's serial), so a NaN that
// stays on screen is one line, not one per sampled frame. These bugs render without any error: the point is to have
// them in the log, where a watcher reads them, instead of in a screenshot someone has to look at.
// Costs: the copies on the GPU in the sampled frames and WORKERS CPU threads reading them; nothing on the game thread or
// in the replay's recording but the copy commands. Copies beyond RTSTATS_CAP bytes in flight are skipped and counted.
// Formats and decoding are the frame dump's (vk_dump_img.c); multisampled targets, BC block views and 3D slices are
// left out, as the dump leaves them, and so are targets in the feedback loop layout. A pass the replay ends only to
// split its command buffer (vk_exec.c maybe_split) is not copied: its targets are half drawn and a cleared one would
// read as constant. Other ends the game did not ask for (a barrier, the end of a submission) are copied: a constant
// report is a lead to check in a dump, not a verdict.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "nvn_api.h"
#include "vk.h"
#include "vk_exec.h"
#include "platform/platform.h"
#include "vk_dump_internal.h"

#define RTSTATS_CAP (512ull << 20)   // bytes of copies in flight; half the dump's, as this runs for the whole session
#define SEEN_CAP 4096                // distinct reports remembered (a power of two)
#define WORKERS 3                    // checking threads: one fell behind in a race (run rtstats1)
#define CONST_MIN 16                 // smallest side of a target the constant rule looks at: 1x1 and 4x4 reductions
                                     // are constant by nature

int vk_rtstats_on;
static uint32_t every = 60;
static volatile int64_t rs_inflight, pending, skipped, made;   // rs_: the dump has its own inflight
static volatile uint64_t hits[VK_RTSTATS_RULES];

typedef struct Job {
    struct Job *next;
    VkBuffer buf;
    VmaAllocation alloc;
    const uint8_t *map;
    uint64_t bytes, frame, prog;
    const Fmt *f;
    uint32_t w, h;
    char tag[8];
} Job;

static PlatMutex q_lock = PLAT_MUTEX_INIT;
static PlatCond q_cv = PLAT_COND_INIT, idle_cv = PLAT_COND_INIT;
static Job *q_head, *q_tail;
static PlatThread *workers[WORKERS];
static int stopping;
static uint64_t seen[SEEN_CAP];
static int nseen, seen_full;

static uint64_t mix(uint64_t h, uint64_t v) { return (h ^ v) * 0x100000001b3ull; }

// 1 the first time this report key is seen (q_lock held).
static int first_time(int rule, const Job *j) {
    uint64_t k = mix(mix(mix(mix(1469598103934665603ull, (uint64_t)rule), (uint64_t)j->f->vk),
                         (uint64_t)j->w << 32 | j->h), j->prog);
    for (const char *t = j->tag; *t; t++) k = mix(k, (uint8_t)*t);
    if (!k) k = 1;
    for (uint64_t i = k;; i++) {
        uint64_t *s = &seen[i & (SEEN_CAP - 1)];
        if (*s == k) return 0;
        if (!*s) {
            if (nseen >= SEEN_CAP / 2) {   // keep the table sparse: later reports are dropped, once said
                if (!seen_full++) rt_log("rtstats: %d distinct reports, later ones are not logged", nseen);
                return 0;
            }
            *s = k, nseen++;
            return 1;
        }
    }
}

// Constant: every texel's bytes equal the one before (one memcmp of the buffer against itself shifted by a texel).
// NaN and infinity: only formats that can hold them are decoded, texel by texel; the others need nothing more. Decoding
// every texel of every format kept one thread busy past the cap (run rtstats1: 12000 copies skipped in a race, the
// 6144x6144 rgba8sn atlas alone 37M texels).
static void scan(const Job *j) {
    uint64_t nan = 0, inf = 0, n = (uint64_t)j->w * j->h;
    uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;   // bounding box of the NaNs
    size_t b = j->f->bytes;
    int nc = j->f->nc, mode = mode_of(j->f);
    int same = n < 2 || !memcmp(j->map, j->map + b, (size_t)(n - 1) * b);
    int floats = mode == M_FLOAT || j->f->vk == VK_FORMAT_D32_SFLOAT || j->f->vk == VK_FORMAT_D32_SFLOAT_S8_UINT;
    float first[4], o[4];
    decode(j->f, j->map, first);
    for (uint32_t y = 0; floats && y < j->h; y++)
        for (uint32_t x = 0; x < j->w; x++) {
            decode(j->f, j->map + ((uint64_t)y * j->w + x) * b, o);
            int has_nan = 0, has_inf = 0;
            for (int c = 0; c < nc; c++) has_nan |= isnan(o[c]), has_inf |= isinf(o[c]);
            if (has_nan) {
                if (!nan++) x0 = x1 = x, y0 = y1 = y;
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                y1 = y;
            }
            inf += has_inf;
        }
    char fn[16], head[160];
    snprintf(head, sizeof head, "frame %llu prog %llu %s %s %ux%u", (unsigned long long)j->frame,
             (unsigned long long)j->prog, j->tag, fmt_name(j->f->vk, fn), j->w, j->h);
    int rep[VK_RTSTATS_RULES] = {nan > 0, inf > 0, same && !nan && j->w >= CONST_MIN && j->h >= CONST_MIN};
    plat_mutex_lock(&q_lock);
    for (int r = 0; r < VK_RTSTATS_RULES; r++) {
        if (!rep[r]) continue;
        __atomic_add_fetch(&hits[r], 1, __ATOMIC_RELAXED);
        if (!first_time(r, j)) continue;
        if (r == VK_RTSTATS_NAN)
            rt_log("rtstats: %s: nan in %llu of %llu texels, %u,%u-%u,%u", head, (unsigned long long)nan,
                   (unsigned long long)n, x0, y0, x1, y1);
        else if (r == VK_RTSTATS_INF)
            rt_log("rtstats: %s: inf in %llu of %llu texels", head, (unsigned long long)inf, (unsigned long long)n);
        else
            rt_log("rtstats: %s: constant %g %g %g %g", head, first[0], first[1], first[2], first[3]);
    }
    plat_mutex_unlock(&q_lock);
}

static void worker(void *arg) {
    (void)arg;
    VmaAllocator vma = dvma();
    for (;;) {
        plat_mutex_lock(&q_lock);
        while (!q_head && !stopping) plat_cond_wait(&q_cv, &q_lock);
        if (!q_head) { plat_mutex_unlock(&q_lock); return; }
        Job *j = q_head;
        if (!(q_head = j->next)) q_tail = NULL;
        plat_mutex_unlock(&q_lock);
        vmaInvalidateAllocation(vma, j->alloc, 0, VK_WHOLE_SIZE);
        scan(j);
        vmaDestroyBuffer(vma, j->buf, j->alloc);
        plat_add64(&rs_inflight, -(int64_t)j->bytes);
        free(j);
        plat_mutex_lock(&q_lock);
        if (!--pending) plat_cond_broadcast(&idle_cv);
        plat_mutex_unlock(&q_lock);
    }
}

// Completion thread: hands the job over and returns (the decoding would hold back the callbacks queued after it,
// among them the counter reports the game waits on).
static void job_ready(void *arg) {
    Job *j = arg;
    plat_mutex_lock(&q_lock);
    if (q_tail) q_tail->next = j;
    else q_head = j;
    q_tail = j;
    plat_cond_signal(&q_cv);
    plat_mutex_unlock(&q_lock);
}

static int sampled(void) {
    uint64_t f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + 1;
    return f % every == 0;
}

static void copy(VkExec *ex, VkImage image, VkFormat vf, VkImageAspectFlags aspect, uint32_t level, uint32_t layer,
                 uint32_t w, uint32_t h, const char *tag) {
    const Fmt *f = fmt_of(vf);
    if (!f || !w || !h) return;
    if (aspect & VK_IMAGE_ASPECT_DEPTH_BIT) aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    else if (aspect != VK_IMAGE_ASPECT_COLOR_BIT) return;
    uint64_t bytes = (uint64_t)w * h * f->bytes;
    if ((uint64_t)rs_inflight + bytes > RTSTATS_CAP) {
        int64_t s = plat_add64(&skipped, 1);
        if (s == 1 || s % 1000 == 0)
            rt_log("rtstats: %lld of %lld copies skipped so far: over %llu MB in flight", (long long)s,
                   (long long)(s + made), RTSTATS_CAP >> 20);
        return;
    }
    Job *j = calloc(1, sizeof *j);
    if (!j) rt_fatal("rtstats: out of memory");
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bytes,
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo info;
    if (vmaCreateBuffer(dvma(), &bi, &ai, &j->buf, &j->alloc, &info) != VK_SUCCESS || !info.pMappedData) {
        free(j);
        return;
    }
    j->map = info.pMappedData, j->bytes = bytes, j->f = f, j->w = w, j->h = h;
    j->frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + 1;
    j->prog = ex->prog ? ex->prog->serial : 0;
    snprintf(j->tag, sizeof j->tag, "%s", tag);
    // The same barriers as the dump's copy_region: the pass's writes before the copy, the copy before the host read.
    VkMemoryBarrier2 pre = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                            .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                            .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                            .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                            .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT};
    VkMemoryBarrier2 post = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                             .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                             .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                             .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                             .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &pre};
    vkCmdPipelineBarrier2(ex->cb, &di);
    VkBufferImageCopy r = {.imageSubresource = {aspect, level, layer, 1}, .imageExtent = {w, h, 1}};
    vkCmdCopyImageToBuffer(ex->cb, image, VK_IMAGE_LAYOUT_GENERAL, j->buf, 1, &r);
    di.pMemoryBarriers = &post;
    vkCmdPipelineBarrier2(ex->cb, &di);
    ex->recorded += 3;
    plat_add64(&rs_inflight, (int64_t)bytes);
    plat_add64(&made, 1);
    plat_mutex_lock(&q_lock);
    pending++;
    plat_mutex_unlock(&q_lock);
    vk_completion_add(ex->fence, job_ready, j);
}

// fbl: the target is attached in the feedback loop layout, which a copy cannot read (vk_exec.c barrier_to takes it
// back to GENERAL later): left out.
static void target(VkExec *ex, const VkExecTarget *t, const char *tag, int fbl) {
    if (!t->image || t->samples > 1 || t->blocks || t->is3d || fbl) return;
    copy(ex, t->image, t->format, t->aspect, t->level, t->base_layer, t->width, t->height, tag);
}

void vk_rtstats_pass_end(VkExec *ex) {
    if (!sampled()) return;
    for (int i = 0; i < 8; i++) {
        char tag[8];
        snprintf(tag, sizeof tag, "t%d", i);
        target(ex, &ex->color[i], tag, ex->fbl_mask >> i & 1);
    }
    target(ex, &ex->depth, "td", ex->fbl_mask >> 8 & 1);
}

// The storage images the dispatch writes, found as vk_dump_dispatch finds them.
void vk_rtstats_dispatch(VkExec *ex, const VkPipeInfo *info) {
    if (!sampled()) return;
    for (uint32_t i = 0; i < info->n_bindings; i++) {
        const VkPipeBinding *bd = &info->bindings[i];
        if (!bd->res || bd->kind != GFX_RES_IMG || !(bd->res->flags & 4) || bd->type != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
            continue;
        int slot = bd->res->slot;
        uint64_t h = slot >= 0 && slot < 32 ? ex->img[5][slot] : 0;
        view_t v;
        int hv = 0;
        const VkTexImage *t = h ? vk_texture_by_id(NVN_HANDLE_TEX(h), &v, &hv) : NULL;
        if (!t || t->samples > 1 || t->type == VK_IMAGE_TYPE_3D) continue;
        uint32_t lv = hv && v.set_levels ? v.base_level : 0, layer = hv && v.set_layers ? v.min_layer : 0;
        if (lv >= t->levels || layer >= t->layers) continue;
        char tag[8];
        snprintf(tag, sizeof tag, "img%d", slot);
        copy(ex, t->image, t->format, t->aspect, lv, layer, t->width >> lv ? t->width >> lv : 1,
             t->height >> lv ? t->height >> lv : 1, tag);
    }
}

void vk_rtstats_every(uint32_t n) {
    every = n ? n : 1;
    if (vk_rtstats_on) return;
    for (int i = 0; i < WORKERS; i++)
        if ((workers[i] = plat_thread_new(worker, NULL, "rtstats"))) vk_rtstats_on = 1;
    if (!vk_rtstats_on) rt_log("rtstats: cannot start its threads, off");
}

void vk_rtstats_init(void) {
    const char *e = cfg_get("MKW_RTSTATS");
    if (!e || !*e || !strcmp(e, "0")) return;
    long n = strtol(e, NULL, 10);
    vk_rtstats_every(n > 0 ? (uint32_t)n : 60);
    if (vk_rtstats_on) rt_log("rtstats: targets checked one frame in %u (nan, inf, constant)", every);
}

void vk_rtstats_flush(void) {
    vk_sync_finish();
    plat_mutex_lock(&q_lock);
    while (pending) plat_cond_wait(&idle_cv, &q_lock);
    plat_mutex_unlock(&q_lock);
}

uint64_t vk_rtstats_hits(int rule) { return __atomic_load_n(&hits[rule], __ATOMIC_RELAXED); }

// After the completion thread has delivered every copy: finish the queued scans before freeing their allocator.
void vk_rtstats_shutdown(void) {
    plat_mutex_lock(&q_lock);
    stopping = 1;
    plat_cond_broadcast(&q_cv);
    plat_mutex_unlock(&q_lock);
    for (int i = 0; i < WORKERS; i++) {
        if (workers[i]) plat_thread_join(workers[i]);
        workers[i] = NULL;
    }
    vk_rtstats_on = stopping = 0;
}
