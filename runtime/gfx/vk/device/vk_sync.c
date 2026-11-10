// Submission, fences and the completion thread of the Vulkan back end.
// One timeline semaphore (VkCtx.timeline) orders everything: each submission records its command handles into a
// primary command buffer (vk_exec_record) and signals the next timeline value, which is the NVN fence value
// the front end hands to syncs. The completion thread waits on the timeline and, in submit order, runs the CPU side
// effects queued for each value (event writes, counter reports, sync state), then frees the memory and texture objects whose users
// completed. A value counts as done only after its side effects ran, so a guest woken by fence_wait sees them.
//
// The submission shape (one timeline value per submission, signalled at ALL_COMMANDS, vkWaitSemaphores for waits, a
// failed wait treated as a lost device) follows LAS Recomp runtime/nvn/vulkan/exec.c:39-110, 355-397
// (https://github.com/peiikai/LASRecomp @ da35b83a9611d046a9a681aae668d7085954d37f, MIT, Copyright (c) 2026 Peiikai).
// Side effects written after GPU completion, as MacNativeGames does: games/totk/third_party/d2r_gfx/gfx/nvn/hle_nvn.c:7-9
// (https://github.com/ly05010419/MacNativeGames @ b75be8cfa704df3701e86130c8ced268e79d6b11, MIT, Copyright (c) 2026
// jhaizhou; origin d2r-mobile).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "gfx.h"
#include "cfg.h"
#include "host.h"
#include "nvn_api.h"
#include "vk.h"
#include "vk_sync_ts.h"
#include "platform/platform.h"

#define WEAK __attribute__((weak))

typedef struct { uint64_t fence; void (*fn)(void *); void *arg; } Pending;
typedef struct { VkCommandBuffer cb; uint64_t fence; } CmdSlot;   // reusable once the timeline reached fence

static PlatMutex start_lock = PLAT_MUTEX_INIT;
static int started;
static PlatThread *completion_thread;

// Submissions: one at a time, so timeline values are signalled in the order they are handed out.
static PlatMutex sub_lock = PLAT_MUTEX_INIT;
static VkCommandPool cmd_pool;
static CmdSlot *slots;
static int nslots;
static int cur_slot;          // the slot vk_sync_submit records into now (an index: take_slot may move slots)
static uint64_t cur_fence;    // the value of the submission being recorded
// Every vkQueueSubmit2 here (a part of a submission or its last one) also signals the next value of part_sem: what is
// released or idle while a submission is recorded waits for the parts already sent, not for the whole submission
// (vk_mem_note_submit, vk_mem_defer_idle). With the chunked recording the first part goes early, and tying those to the
// value still being recorded kept them alive through it: ht1 died loading the race, staging out of memory with 852 to
// 5 250 objects waiting for a value no wait could reach.
static VkSemaphore part_sem;
static uint64_t part_val;     // newest part value submitted
// MKW_WB_ORDER (vk_mem_wback.c): a part writing bytes of an earlier submission whose write-back is still to come is
// handed to the queue once that value is done (done advances after its write-backs ran). The replay thread waits, not
// the GPU: a GPU wait on a value the CPU signals later made RADV defer the queue's work and a sparse unmap after it
// failed ("Failed to replace a PRT VA region (-22)", device lost at the title, tdiff1).
static uint64_t wb_waits, wb_skips, wb_ns;   // sub_lock: parts that waited since the last log, waits already
                                             // satisfied, time waited

static void wb_wait(uint64_t fence) {   // sub_lock held, before the queue lock
    uint64_t need = vk_mem_wb_need_take(fence);
    if (!need) return;
    if (need <= vk_sync_fence_done()) {
        wb_skips++;
        return;
    }
    uint64_t t0 = kern_now_ns();
    // Submitted already (values reach the queue in order); the cap is against a lock the completion thread's write-backs
    // would need being held here.
    if (!vk_sync_fence_wait(need, 500000000ull))
        rt_log("vk: MKW_WB_ORDER: the write-back of %llu did not come in 500 ms; part of %llu sent without it",
               (unsigned long long)need, (unsigned long long)fence);
    wb_ns += kern_now_ns() - t0;
    wb_waits++;
}

// Shared with the completion thread (st_lock).
static PlatMutex st_lock = PLAT_MUTEX_INIT;
static PlatCond cv_work = PLAT_COND_INIT;   // a submission, a callback or stop
static PlatCond cv_done = PLAT_COND_INIT;   // done advanced
static uint64_t issued, done;     // newest submitted value; newest value whose side effects ran
static uint64_t issued_part;      // the part value of issued's last part (MKW_GPU_GUARD)
static uint64_t queued;           // newest value with a part on the GPU queue (MKW_EARLY_READ: what may be running)
static uint64_t reserved;         // asynchronous replay: newest value handed out before its submission (>= issued)
static int stop;
static Pending *queue;            // side effects in the order they were added
static int qhead, qn, qcap;

// MKW_SUBMIT_THREAD (default 1; "ab" in the B blocks only): a part's vkQueueSubmit2 (vk_sync_split_with) is not made
// on the replay thread, the race frame's critical path, but by the thread "vk submit": in the race ~1,700 parts every
// 1,200 submissions spent ~40 ms a second there, a quarter of the replay thread's time in the kernel (kfp1, kfp2). The
// part takes its place in queue order on the replay thread (sparse order, under the queue lock) and waits in parts[];
// whoever takes the queue lock next submits the waiting parts first (vk_queue_lock), so every other queue operation
// (upload batches, sparse binds, the present, the submission's last part) still comes after them.
typedef struct {
    VkCommandBufferSubmitInfo cbi[2 + VK_SPLIT_EXTRA];
    VkSemaphoreSubmitInfo wait, sig[2];
    VkSubmitInfo2 si;
    uint64_t fence;
} PendPart;
#define PEND_CAP 32
static PendPart parts[PEND_CAP];   // queue lock
static int pend_head, pend_n;      // queue lock; pend_n also read without it (atomic)
static PlatMutex pend_mx = PLAT_MUTEX_INIT;
static PlatCond pend_cv = PLAT_COND_INIT;
static PlatThread *submit_thread;
static int submit_on = -1;
static _Thread_local int in_flush, is_submitter;
static uint64_t pend_by_thread, pend_by_other, pend_ns;   // queue lock: parts sent by each, time in vkQueueSubmit2

static void part_sent(uint64_t fence, VkResult r) {   // queue lock held
    vk_tl_queue_submit(fence);
    __atomic_store_n(&queued, fence, __ATOMIC_RELEASE);
    if (r == VK_ERROR_DEVICE_LOST) vk_exec_recent_log();
    if (r != VK_SUCCESS) rt_fatal("vk: vkQueueSubmit2 of a part of value %llu failed (%d)", (unsigned long long)fence, (int)r);
}

// vk_queue_lock, right after taking it: the waiting parts go to the queue, in order, before what the caller submits.
void vk_sync_parts_flush(void) {
    if (!__atomic_load_n(&pend_n, __ATOMIC_ACQUIRE) || in_flush) return;
    in_flush = 1;
    VkQueue q = vk_ctx()->gfx_queue;
    while (pend_n) {
        PendPart *p = &parts[pend_head];
        uint64_t t0 = kern_now_ns();
        VkResult r = vkQueueSubmit2(q, 1, &p->si, VK_NULL_HANDLE);
        pend_ns += kern_now_ns() - t0;
        if (is_submitter) pend_by_thread++;
        else pend_by_other++;
        part_sent(p->fence, r);
        pend_head = (pend_head + 1) % PEND_CAP;
        __atomic_store_n(&pend_n, pend_n - 1, __ATOMIC_RELEASE);
    }
    in_flush = 0;
}

static void submit_main(void *arg) {
    (void)arg;
    is_submitter = 1;
    plat_mutex_lock(&pend_mx);
    for (;;) {
        while (!__atomic_load_n(&pend_n, __ATOMIC_ACQUIRE)) plat_cond_wait(&pend_cv, &pend_mx);
        plat_mutex_unlock(&pend_mx);
        vk_queue_lock();   // sends them
        vk_queue_unlock();
        plat_mutex_lock(&pend_mx);
    }
}

// Queue lock held, the part's order taken: parts[] gets it, or the queue now when it is full.
static void part_defer(const VkSubmitInfo2 *si, const VkCommandBufferSubmitInfo *cbi, int ncbi,
                       const VkSemaphoreSubmitInfo *sig, uint64_t fence) {
    if (!submit_thread && !(submit_thread = plat_thread_new(submit_main, NULL, "vk submit")))
        rt_fatal("vk: cannot start the submit thread");
    if (pend_n == PEND_CAP) vk_sync_parts_flush();
    PendPart *p = &parts[(pend_head + pend_n) % PEND_CAP];
    memcpy(p->cbi, cbi, sizeof *cbi * (size_t)ncbi);
    p->si = *si;
    p->si.pCommandBufferInfos = p->cbi;
    if (si->waitSemaphoreInfoCount) p->wait = *si->pWaitSemaphoreInfos, p->si.pWaitSemaphoreInfos = &p->wait;
    memcpy(p->sig, sig, sizeof *sig * si->signalSemaphoreInfoCount);
    p->si.pSignalSemaphoreInfos = p->sig;
    p->fence = fence;
    __atomic_store_n(&pend_n, pend_n + 1, __ATOMIC_RELEASE);
    plat_mutex_lock(&pend_mx);
    plat_cond_signal(&pend_cv);
    plat_mutex_unlock(&pend_mx);
}

// Weak stand-in for vk_exec.c's replay: only empty recordings ({OP_BEGIN, OP_END}) can be submitted.
WEAK int vk_exec_record(VkCommandBuffer cb, const uint64_t *handles, int n, uint64_t fence) {
    (void)cb, (void)fence;
    for (int i = 0; i < n; i++) {
        const op_hdr *h = handles[i] ? nvn_op_first(handles[i]) : NULL;
        if (!h || h->op != OP_BEGIN || h->size < sizeof *h || nvn_op_next(h)->op != OP_END)
            rt_fatal("vk: op replay is not linked");
    }
    return 0;
}

// MKW_TRACE_TL: the completion thread's side effects by callback, run between a value being reached and its waiters
// being released (tl-se1: 1.5 ms a frame on the race frame's critical path). Every 3000 values, the costliest.
static struct { void (*fn)(void *); uint64_t ns, n; } cb_cost[32];
static uint64_t cb_values;

static void cb_note(void (*fn)(void *), uint64_t ns) {   // st_lock not held; completion thread only
    int i = 0;
    while (i < 32 && cb_cost[i].fn && cb_cost[i].fn != fn) i++;
    if (i == 32) return;
    cb_cost[i].fn = fn, cb_cost[i].ns += ns, cb_cost[i].n++;
}

static void cb_log(uint64_t values) {
    cb_values += values;
    if (cb_values < 3000) return;
    char buf[1200];
    int k = 0;
    for (int i = 0; i < 32 && cb_cost[i].fn && k < (int)sizeof buf - 80; i++)
        k += snprintf(buf + k, sizeof buf - (size_t)k, "; build/mkw+%#llx %.3f ms (%llu)",
                      (unsigned long long)((uintptr_t)cb_cost[i].fn - plat_exe_base()),
                      cb_cost[i].ns / 1e6 / (double)cb_values, (unsigned long long)cb_cost[i].n);
    rt_log("vk: completion side effects per value over %llu values (MKW_TRACE_TL, tools/run/sym.py)%s",
           (unsigned long long)cb_values, buf);
    memset(cb_cost, 0, sizeof cb_cost), cb_values = 0;
}

static void completion_main(void *arg) {
    VkCtx *c = arg;
    plat_mutex_lock(&st_lock);
    for (;;) {
        while (!stop && done == issued && !(qn && queue[qhead].fence <= done))
            plat_cond_wait(&cv_work, &st_lock);
        if (stop) break;
        uint64_t reached = done;
        if (issued > done) {
            uint64_t want = done + 1;
            plat_mutex_unlock(&st_lock);
            VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1,
                                      .pSemaphores = &c->timeline, .pValues = &want};
            VkResult r = vkWaitSemaphores(c->device, &wi, 100000000ull);   // 100 ms: stop is checked between waits
            if (r == VK_ERROR_DEVICE_LOST) vk_exec_recent_log();
            if (r != VK_SUCCESS && r != VK_TIMEOUT) rt_fatal("vk: vkWaitSemaphores failed (%d; device lost?)", (int)r);
            vkGetSemaphoreCounterValue(c->device, c->timeline, &reached);
            plat_mutex_lock(&st_lock);
            if (reached > issued) reached = issued;
            if (reached > done) vk_tl_reached(done, reached, 0);
        }
        while (qn && queue[qhead].fence <= reached) {
            Pending p = queue[qhead++];
            qn--;
            plat_mutex_unlock(&st_lock);
            if (vk_tl_on() > 0) {
                uint64_t t0 = kern_now_ns();
                p.fn(p.arg);
                cb_note(p.fn, kern_now_ns() - t0);
            } else {
                p.fn(p.arg);
            }
            plat_mutex_lock(&st_lock);
        }
        if (reached > done) {
            // Waiters are released first: freeing what the completed work used is not a side effect the guest sees,
            // and the game's main thread waits on these fences (nvnSyncWait 40% of it in the cup movie, live33).
            uint64_t was = done;
            __atomic_store_n(&done, reached, __ATOMIC_RELEASE);
            plat_cond_broadcast(&cv_done);
            plat_mutex_unlock(&st_lock);
            vk_tl_reached(was, reached, 1);
            if (vk_tl_on() > 0) cb_log(reached - was);
            vk_mem_collect();
            plat_mutex_lock(&st_lock);
        }
    }
    plat_mutex_unlock(&st_lock);
    return;
}

void vk_sync_start(void) {
    if (__atomic_load_n(&started, __ATOMIC_ACQUIRE)) return;
    plat_mutex_lock(&start_lock);
    if (!started) {
        if (!vk_init()) rt_fatal("vk: no usable Vulkan device (see the vk: lines above; MKW_GPU=null runs without a GPU)");
        VkCtx *c = vk_ctx();
        VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                      .queueFamilyIndex = c->gfx_family};
        if (vkCreateCommandPool(c->device, &pi, NULL, &cmd_pool) != VK_SUCCESS) rt_fatal("vk: submission command pool");
        vk_ts_start(c);
        uint64_t now = 0;
        vkGetSemaphoreCounterValue(c->device, c->timeline, &now);
        if (issued > now) {   // a new device after vk_shutdown: values keep increasing for the front end
            VkSemaphoreSignalInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, .semaphore = c->timeline,
                                        .value = issued};
            if (vkSignalSemaphore(c->device, &si) != VK_SUCCESS) rt_fatal("vk: timeline restart at %llu", (unsigned long long)issued);
            now = issued;
        }
        plat_mutex_lock(&st_lock);
        issued = done = now;
        if (reserved < now) reserved = now;
        stop = 0;
        plat_mutex_unlock(&st_lock);
        if (!part_sem) {
            VkSemaphoreTypeCreateInfo ti = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                            .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = part_val};
            VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &ti};
            if (vkCreateSemaphore(c->device, &si, NULL, &part_sem) != VK_SUCCESS) rt_fatal("vk: part semaphore");
        }
        vk_mem_note_submit(part_val);
        completion_thread = plat_thread_new(completion_main, c, "vk completion");
        if (!completion_thread) rt_fatal("vk: cannot start the completion thread");
        __atomic_store_n(&started, 1, __ATOMIC_RELEASE);
        vk_astc_mode();   // decides MKW_ASTC and builds the ASTC decoder now, not at the first ASTC upload
    }
    plat_mutex_unlock(&start_lock);
}

// The index of a command buffer the GPU no longer uses, claimed for value `fence`. Called with sub_lock held; may move
// slots[].
static int claim_slot(VkCtx *c, uint64_t fence) {
    uint64_t gpu = 0;
    vkGetSemaphoreCounterValue(c->device, c->timeline, &gpu);
    for (int i = 0; i < nslots; i++)
        if (slots[i].fence <= gpu) {
            slots[i].fence = fence;
            return i;
        }
    CmdSlot *n = realloc(slots, sizeof *n * (size_t)(nslots + 1));
    if (!n) rt_fatal("vk: out of memory (command buffers)");
    slots = n;
    CmdSlot *s = &slots[nslots];
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cmd_pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    if (vkAllocateCommandBuffers(c->device, &ai, &s->cb) != VK_SUCCESS) rt_fatal("vk: command buffer allocation");
    s->fence = fence;
    return nslots++;
}

static CmdSlot *take_slot(VkCtx *c, uint64_t fence) {
    cur_slot = claim_slot(c, fence);
    return &slots[cur_slot];
}

// The copies the replay's first accesses to VRAM mirrors queued (vk_memory.c), in a command buffer of their own that is
// submitted right before the part that needs them and timed like a part; VK_NULL_HANDLE when there are none. After the
// part was recorded, sub_lock held; may move slots[].
// First brings the shadow storage up to date with the CPU's writes, which needs no command buffer.
static uint64_t fe_waits, fe_ns;   // sub_lock: MKW_FENCE_EARLY's waits before a shadow copy, since the last log

static VkCommandBuffer prologue(VkCtx *c, uint64_t fence) {
    // MKW_FENCE_EARLY: the game was told a fence was done once recorded and may now rewrite what that GPU work still
    // reads; the copy into shadow storage that would carry those writes waits for it. As wb_wait, under sub_lock: the
    // completion thread does not take it.
    // MKW_FENCE_EARLY_GUARD: 1 this wait before every part (fe1: 7.7 ms each, ~0.6 a frame); 0 none (a measurement:
    // fe3 found 20 copies over bytes a running submission read); unset or 2 the precise one, only the copies that change
    // such bytes wait (vk_mem_race.c race_wait), which also covers the shadow thread's copies.
    static int guard = -1;
    if (guard < 0) guard = cfg_get("MKW_FENCE_EARLY_GUARD") ? atoi(cfg_get("MKW_FENCE_EARLY_GUARD")) : 2;
    uint64_t rel = __atomic_load_n(&mem_early_rel, __ATOMIC_ACQUIRE);
    if (mem_fence_early && guard == 1 && rel > vk_sync_fence_done()) {
        uint64_t t0 = kern_now_ns();
        vk_sync_fence_wait(rel, 2000000000ull);
        fe_waits++, fe_ns += kern_now_ns() - t0;
    }
    vk_mem_shadow_sync(fence);   // shadow storage: the CPU's writes go in before the part is submitted
    if (!vk_mem_mirror_pending()) return VK_NULL_HANDLE;
    int i = claim_slot(c, fence);   // before reading slots: the claim may move it
    VkCommandBuffer cb = slots[i].cb;
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cb, &bi);
    if (vk_guard_on()) vk_guard_cb_begin(cb);
    int pair = vk_ts_open(cb);
    vk_mem_mirror_prologue(cb);
    vk_ts_close(cb, pair, fence);
    vkEndCommandBuffer(cb);
    return cb;
}

// MKW_TRACE=perf: where a submission's time goes, summed per log line (every 1200 submissions): waiting for sub_lock
// (another thread's submission), the walk and recording, the uploads, the shadow sync and the queue submit.
// The split's last stretch is told apart: vkQueueSubmit2 itself, claiming the next buffer and beginning it.
enum { SS_LOCK, SS_RECORD, SS_RECORD_CPU, SS_WALK, SS_UPLOAD, SS_PROLOGUE, SS_QUEUE, SS_SPLIT_END, SS_SPLIT_PRO, SS_SPLIT_Q,
       SS_SPLIT_SUBMIT2, SS_SPLIT_CLAIM, SS_SPLIT_BEGIN, SS_N };

// The recording's CPU time on its thread: against its wall time, how long the thread was off the CPU meanwhile.
static uint64_t thread_cpu_ns(void) { return plat_self_cpu_ns(); }
static uint64_t ss_ns[SS_N], ss_subs, ss_t0;   // sub_lock, except ss_ns[SS_LOCK]'s add (atomic)
static uint64_t ss_splits;                      // sub_lock: parts submitted since the last line

static void ss_log(void) {
    static const char *const name[SS_N] = {"waiting for another thread's submission", "recording", "of it on the CPU",
                                           "of it in vk_exec_record", "uploads", "shadow sync and prologue", "queue submit",
                                           "splits: recorder drain and end", "splits: uploads, shadow sync, prologue",
                                           "splits: queue submit and next buffer", "of it vkQueueSubmit2",
                                           "of it claiming the next buffer", "of it beginning it"};
    uint64_t now = kern_now_ns(), span = ss_t0 ? now - ss_t0 : 0;
    ss_t0 = now;
    if (span) {
        char buf[1024];
        int k = 0;
        for (int i = 0; i < SS_N; i++)
            k += snprintf(buf + k, sizeof buf - (size_t)k, "%s%s %.1f", i ? ", " : "", name[i],
                          __atomic_load_n(&ss_ns[i], __ATOMIC_RELAXED) * 1e3 / (double)span);
        rt_log("perf: submit time in ms per s of wall clock over %llu submissions and %llu parts: %s",
               (unsigned long long)ss_subs, (unsigned long long)ss_splits, buf);
        vk_queue_lock();
        if (pend_by_thread || pend_by_other)
            rt_log("perf: parts sent by the submit thread %llu, by others taking the queue lock %llu, vkQueueSubmit2 "
                   "%.1f ms per s (MKW_SUBMIT_THREAD)", (unsigned long long)pend_by_thread,
                   (unsigned long long)pend_by_other, pend_ns * 1e3 / (double)span);
        pend_by_thread = pend_by_other = pend_ns = 0;
        vk_queue_unlock();
        if (plat_lock_report(0, buf, sizeof buf)) rt_log("perf: lock waits since the last line (MKW_LOCK_PROF):%s", buf);
        if (plat_lock_report(1, buf, sizeof buf))
            rt_log("perf: lock holds a thread slept on since the last line (MKW_LOCK_PROF):%s", buf);
    }
    for (int i = 0; i < SS_N; i++) __atomic_store_n(&ss_ns[i], 0, __ATOMIC_RELAXED);
    ss_subs = ss_splits = 0;
}

VkCommandBuffer vk_sync_split(void) { return vk_sync_split_with(NULL, 0); }

VkCommandBuffer vk_sync_split_with(const VkCommandBuffer *extra, int nextra) {
    VkCtx *c = vk_ctx();
    VkCommandBuffer cb = slots[cur_slot].cb;
    int ss = (rt_trace & RT_TRACE_PERF) != 0;
    uint64_t q0 = ss ? kern_now_ns() : 0, q1;
    rec_end();   // every command of the part is in cb
    vk_ts_end(nextra ? extra[nextra - 1] : cb, cur_fence);   // the part's GPU time spans the chunks after it
    vkEndCommandBuffer(cb);
    for (int i = 0; i < nextra; i++) vkEndCommandBuffer(extra[i]);
    if (ss) q1 = kern_now_ns(), ss_ns[SS_SPLIT_END] += q1 - q0, q0 = q1;
    vk_queue_lock();
    vk_sparse_order(cur_fence, 2);   // its sparse binds first: the uploads may copy into the levels they bind
    vk_queue_unlock();
    vk_upload_flush(0);   // the uploads this part asked for precede it in queue order
    VkCommandBuffer pro = prologue(c, cur_fence);
    if (ss) q1 = kern_now_ns(), ss_ns[SS_SPLIT_PRO] += q1 - q0, q0 = q1;
    VkCommandBufferSubmitInfo cbi[2 + VK_SPLIT_EXTRA] = {
        {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = pro},
        {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cb}};
    if (nextra > VK_SPLIT_EXTRA) rt_fatal("vk: %d command buffers after a part (at most %d)", nextra, VK_SPLIT_EXTRA);
    for (int i = 0; i < nextra; i++)
        cbi[2 + i] = (VkCommandBufferSubmitInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = extra[i]};
    VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = (pro ? 2u : 1u) + (uint32_t)nextra,
                        .pCommandBufferInfos = pro ? cbi : cbi + 1};
    VkSemaphoreSubmitInfo sig[2] = {{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = part_sem,
                                     .value = part_val + 1, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT}};
    wb_wait(cur_fence);
    vk_queue_lock();
    VkSparseOrder so = vk_sparse_order(cur_fence, 1);   // sparse binds made for it, behind every earlier submission
    VkSemaphoreSubmitInfo bw = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = so.wsem, .value = so.wait,
                                .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT},
                          os = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = so.ssem, .value = so.signal,
                                .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    if (so.wsem && (so.wait || so.wsem != so.ssem)) si.waitSemaphoreInfoCount = 1, si.pWaitSemaphoreInfos = &bw;
    if (so.ssem) sig[1] = os;
    si.signalSemaphoreInfoCount = so.ssem ? 2 : 1, si.pSignalSemaphoreInfos = sig;
    uint64_t k0 = ss ? kern_now_ns() : 0;
    if (submit_on < 0) submit_on = rt_knob("SUBMIT_THREAD", "1");
    if (RT_KNOB_ON(submit_on)) {
        part_defer(&si, pro ? cbi : cbi + 1, (int)si.commandBufferInfoCount, sig, cur_fence);
    } else {
        VkResult r = vkQueueSubmit2(c->gfx_queue, 1, &si, VK_NULL_HANDLE);
        part_sent(cur_fence, r);
    }
    if (ss) ss_ns[SS_SPLIT_SUBMIT2] += kern_now_ns() - k0, ss_splits++;
    vk_queue_unlock();
    vk_mem_note_submit(++part_val);   // objects destroyed from here on may be in use by the part just submitted
    if (ss) k0 = kern_now_ns();
    CmdSlot *s = take_slot(c, cur_fence);   // the submitted part keeps its slot until the value is reached
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if (ss) q1 = kern_now_ns(), ss_ns[SS_SPLIT_CLAIM] += q1 - k0, k0 = q1;
    vkBeginCommandBuffer(s->cb, &bi);
    if (ss) ss_ns[SS_SPLIT_BEGIN] += kern_now_ns() - k0;
    if (vk_guard_on()) vk_guard_cb_begin(s->cb);
    vk_ts_begin(s->cb);
    rec_begin(s->cb);
    if (ss) ss_ns[SS_SPLIT_Q] += kern_now_ns() - q0;
    return s->cb;
}

// Asynchronous replay (vk_backend.c): the value the next queued submission or present will signal. Values are handed
// out in queue order and the replay thread submits them in that order, so each vk_sync_submit gets exactly its value.
uint64_t vk_sync_reserve(void) {
    vk_sync_start();
    plat_mutex_lock(&st_lock);
    if (reserved < issued) reserved = issued;
    uint64_t v = ++reserved;
    plat_mutex_unlock(&st_lock);
    return v;
}

uint64_t vk_sync_submit(const uint64_t *handles, int n) {
    vk_sync_start();
    VkCtx *c = vk_ctx();
    int ss = (rt_trace & RT_TRACE_PERF) != 0;
    uint64_t q0 = ss ? kern_now_ns() : 0, q1;
    plat_mutex_lock(&sub_lock);
    uint64_t c0 = 0;
    if (ss) q1 = kern_now_ns(), __atomic_fetch_add(&ss_ns[SS_LOCK], q1 - q0, __ATOMIC_RELAXED), q0 = q1, c0 = thread_cpu_ns();
    vk_mem_dirty_submit();   // MKW_TRACE=perf: what a mirror would upload before this submit
    uint64_t fence = __atomic_load_n(&issued, __ATOMIC_ACQUIRE) + 1;
    cur_fence = fence;
    vk_mem_recording(fence);
    CmdSlot *s = take_slot(c, fence);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    uint64_t t0 = perf_begin();
    vkBeginCommandBuffer(s->cb, &bi);
    if (vk_guard_on()) vk_guard_cb_begin(s->cb);
    vk_ts_begin(s->cb);
    rec_begin(s->cb);
    int recorded = vk_exec_record(s->cb, handles, n, fence);
    if (ss) ss_ns[SS_WALK] += kern_now_ns() - q0;
    s = &slots[cur_slot];   // the last part (vk_sync_split)
    rec_end();
    if (recorded) vk_ts_end(s->cb, fence);
    else vk_ts_drop();
    vkEndCommandBuffer(s->cb);
    perf_replay(t0);
    if (ss) q1 = kern_now_ns(), ss_ns[SS_RECORD] += q1 - q0, q0 = q1, ss_ns[SS_RECORD_CPU] += thread_cpu_ns() - c0;
    // After the recording: the uploads it asked for (and every earlier one) precede it in queue order, behind the
    // sparse binds it made (the uploads may copy into the levels they bind).
    vk_queue_lock();
    vk_sparse_order(fence, 2);
    vk_queue_unlock();
    vk_upload_flush(0);
    if (ss) q1 = kern_now_ns(), ss_ns[SS_UPLOAD] += q1 - q0, q0 = q1;
    VkCommandBuffer pro = prologue(c, fence);
    if (ss) q1 = kern_now_ns(), ss_ns[SS_PROLOGUE] += q1 - q0, q0 = q1;
    s = &slots[cur_slot];
    VkCommandBufferSubmitInfo cbi[2] = {{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = pro},
                                        {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = s->cb}};
    VkSemaphoreSubmitInfo sig[3] = {{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = c->timeline,
                                     .value = fence, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT},
                                    {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = part_sem,
                                     .value = part_val + 1, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT}};
    VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                        .commandBufferInfoCount = (pro ? 1u : 0u) + (recorded ? 1u : 0u),
                        .pCommandBufferInfos = pro ? cbi : cbi + 1, .signalSemaphoreInfoCount = 3,
                        .pSignalSemaphoreInfos = sig};
    wb_wait(fence);
    vk_queue_lock();
    VkSparseOrder so = vk_sparse_order(fence, 1);   // sparse binds made for it, behind every earlier submission
    VkSemaphoreSubmitInfo bw = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = so.wsem, .value = so.wait,
                                .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    sig[2] = (VkSemaphoreSubmitInfo){.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = so.ssem,
                                     .value = so.signal, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    if (!so.ssem) si.signalSemaphoreInfoCount = 2;
    if (so.wsem && (so.wait || so.wsem != so.ssem)) si.waitSemaphoreInfoCount = 1, si.pWaitSemaphoreInfos = &bw;
    VkResult r = vkQueueSubmit2(c->gfx_queue, 1, &si, VK_NULL_HANDLE);
    vk_queue_unlock();
    vk_tl_queue_submit(fence);
    __atomic_store_n(&queued, fence, __ATOMIC_RELEASE);
    if (r == VK_ERROR_DEVICE_LOST) vk_exec_recent_log();
    if (r != VK_SUCCESS) rt_fatal("vk: vkQueueSubmit2 of value %llu failed (%d)", (unsigned long long)fence, (int)r);
    s->fence = recorded ? fence : 0;
    vk_mem_note_submit(++part_val);
    vk_mem_recording(0);
    plat_mutex_lock(&st_lock);
    issued_part = part_val;
    __atomic_store_n(&issued, fence, __ATOMIC_RELEASE);
    plat_cond_broadcast(&cv_work);
    plat_mutex_unlock(&st_lock);
    static unsigned subs;
    if (++subs % 4096 == 0) {
        rec_stats_log();
        if (wb_waits || wb_skips)
            rt_log("vk: %llu parts waited %.1f ms in all for an earlier write-back, %llu found it done (MKW_WB_ORDER)",
                   (unsigned long long)wb_waits, (double)wb_ns / 1e6, (unsigned long long)wb_skips);
        wb_waits = wb_skips = wb_ns = 0;
        if (mem_fence_early)
            rt_log("vk: %llu parts waited %.1f ms in all for the GPU of a fence released early before their shadow copy "
                   "(MKW_FENCE_EARLY)", (unsigned long long)fe_waits, (double)fe_ns / 1e6);
        fe_waits = fe_ns = 0;
    }
    if (ss) {
        ss_ns[SS_QUEUE] += kern_now_ns() - q0;
        if (++ss_subs >= 1200) ss_log();
    }
    plat_mutex_unlock(&sub_lock);
    return fence;
}

static _Thread_local VkCompletionList *defer_to;

void vk_completion_defer(VkCompletionList *l) { defer_to = l; }

void vk_completion_add(uint64_t fence, void (*fn)(void *), void *arg) {
    if (defer_to) {
        VkCompletionList *l = defer_to;
        if (l->n == l->cap) {
            l->cap = l->cap ? l->cap * 2 : 64;
            if (!(l->e = realloc(l->e, sizeof *l->e * (size_t)l->cap))) rt_fatal("vk: out of memory (completion list)");
        }
        l->e[l->n++] = (VkCompletionEnt){fence, fn, arg};
        return;
    }
    plat_mutex_lock(&st_lock);
    if (qhead + qn == qcap) {
        if (qhead) {
            memmove(queue, queue + qhead, sizeof *queue * (size_t)qn);
            qhead = 0;
        } else {
            int cap = qcap ? qcap * 2 : 256;
            Pending *q = realloc(queue, sizeof *q * (size_t)cap);
            if (!q) rt_fatal("vk: out of memory (completion queue)");
            queue = q, qcap = cap;
        }
    }
    queue[qhead + qn++] = (Pending){fence, fn, arg};
    // The completion thread sleeps only while nothing is issued beyond done: a callback for a later value (the replay's,
    // for the submission it records: counter reports, freed resources) becomes due through the submit, which wakes it.
    if (fence <= done) plat_cond_broadcast(&cv_work);
    plat_mutex_unlock(&st_lock);
}

static int fence_wait_impl(uint64_t value, uint64_t timeout_ns);

int vk_sync_fence_wait(uint64_t value, uint64_t timeout_ns) {
    uint64_t p0 = perf_begin();
    int64_t w0 = vk_tl_on() > 0 && vk_sync_fence_done() < value ? (int64_t)plat_now_ns() : 0;
    int ok = fence_wait_impl(value, timeout_ns);
    if (w0) vk_tl_wait(value, w0, (int64_t)plat_now_ns());
    perf_end(PERF_FENCE, p0);
    perf_gpu_wait(p0);
    return ok;
}

static int fence_wait_impl(uint64_t value, uint64_t timeout_ns) {
    vk_sync_start();
    plat_mutex_lock(&st_lock);
    int ok;
    if (value > issued && value > reserved) {
        ok = 0;   // never submitted nor queued: never reached (as in the null back end)
    } else {
        uint64_t t0 = kern_now_ns();
        while (done < value) {
            if (timeout_ns < (1ull << 62)) {
                uint64_t el = kern_now_ns() - t0;
                if (el >= timeout_ns) break;
                plat_cond_wait_ns(&cv_done, &st_lock, timeout_ns - el);
            } else {
                plat_cond_wait(&cv_done, &st_lock);
            }
        }
        ok = done >= value;
    }
    plat_mutex_unlock(&st_lock);
    return ok;
}

uint64_t vk_sync_fence_done(void) { return __atomic_load_n(&done, __ATOMIC_ACQUIRE); }

uint64_t vk_sync_fence_queued(void) { return __atomic_load_n(&queued, __ATOMIC_ACQUIRE); }

uint64_t vk_sync_issued(void) { return __atomic_load_n(&issued, __ATOMIC_ACQUIRE); }

// MKW_FENCE_EARLY: value is recorded and its last part handed to the queue (issued moves under st_lock and broadcasts
// cv_work). A value the replay will never record (beyond what was handed out) is never reached.
int vk_sync_issued_wait(uint64_t value, uint64_t timeout_ns) {
    vk_sync_start();
    plat_mutex_lock(&st_lock);
    uint64_t t0 = kern_now_ns();
    while (issued < value && value <= reserved) {
        uint64_t el = kern_now_ns() - t0;
        if (el >= timeout_ns) break;
        plat_cond_wait_ns(&cv_work, &st_lock, timeout_ns - el);
    }
    int ok = issued >= value;
    plat_mutex_unlock(&st_lock);
    return ok;
}

uint64_t vk_sync_part_next(void) { return __atomic_load_n(&part_val, __ATOMIC_ACQUIRE) + 1; }

uint64_t vk_sync_issued_pair(uint64_t *part) {
    plat_mutex_lock(&st_lock);
    uint64_t f = issued;
    *part = issued_part;
    plat_mutex_unlock(&st_lock);
    return f;
}

uint64_t vk_sync_part_done(void) {
    VkCtx *c = vk_ctx();
    uint64_t v = 0;
    if (c && part_sem) vkGetSemaphoreCounterValue(c->device, part_sem, &v);
    return v;
}

void vk_sync_part_wait(uint64_t value) {
    VkCtx *c = vk_ctx();
    if (!c || !part_sem) return;
    VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1,
                              .pSemaphores = &part_sem, .pValues = &value};
    VkResult r = vkWaitSemaphores(c->device, &wi, UINT64_MAX);
    if (r == VK_ERROR_DEVICE_LOST) vk_exec_recent_log();
    if (r != VK_SUCCESS) rt_fatal("vk: wait for part %llu failed (%d)", (unsigned long long)value, (int)r);
}

void vk_sync_finish(void) {
    vk_sync_start();
    vk_sync_fence_wait(__atomic_load_n(&issued, __ATOMIC_ACQUIRE), UINT64_MAX);
    vk_upload_flush(1);
}

void vk_sync_shutdown(void) {
    plat_mutex_lock(&start_lock);
    if (started) {
        vk_sync_finish();
        plat_mutex_lock(&st_lock);
        stop = 1;
        plat_cond_broadcast(&cv_work);
        plat_mutex_unlock(&st_lock);
        plat_thread_join(completion_thread);
        completion_thread = NULL;
        plat_mutex_lock(&st_lock);
        qhead = qn = 0;   // callbacks of values never submitted
        plat_mutex_unlock(&st_lock);
        plat_mutex_lock(&sub_lock);
        VkCtx *c = vk_ctx();
        if (c && cmd_pool) vkDestroyCommandPool(c->device, cmd_pool, NULL);
        vk_ts_shutdown(c);
        if (c && part_sem) vkDestroySemaphore(c->device, part_sem, NULL);
        part_sem = VK_NULL_HANDLE;
        cmd_pool = VK_NULL_HANDLE;
        free(slots);
        slots = NULL;
        nslots = 0;
        plat_mutex_unlock(&sub_lock);
        __atomic_store_n(&started, 0, __ATOMIC_RELEASE);
    }
    plat_mutex_unlock(&start_lock);
}
