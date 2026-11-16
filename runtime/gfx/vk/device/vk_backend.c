// The Vulkan graphics back end: gfx_vulkan_backend wires the memory and
// texture layer (vk_memory.c, vk_texture.c), submission and fences (vk_sync.c) and the present. Every entry starts the
// device on first use.
//
// Present: the game texture is resolved on the calling thread and its blit is handed to a present thread, so
// nvnQueuePresentTexture returns at once (section 5). Queue order does the rest: the submissions that wrote the texture
// are already on gfx_queue, and every call that may write or free GPU state the blit reads (submit, finish, pool and
// texture teardown, texel writes) first waits until the queued blit was submitted (vk_present_drain).
//
// Back-edge pacing: the present thread submits the blit at once, then holds the host present (vkQueuePresentKHR) until
// the frame's display time from the pacer (nvn_api_obj.c), so present-to-present intervals stay even whatever the
// replay took. The blit is already on the queue, so the wait blocks no submission. With frame generation
// (vk_framegen.c) the frames made before the game's are blitted with it and presented one step apart, the game's last.
//
// Asynchronous replay (MKW_REPLAY_ASYNC, default 1; 0 turns it off): nvnQueueSubmitCommands
// and nvnQueuePresentTexture only queue. Each item gets its timeline value at enqueue (vk_sync_reserve), in queue
// order; the replay thread runs the synchronous submit and present bodies in that order, so every value is signalled
// by the submission it was handed out for. The game runs at most one frame ahead: queueing present N waits until the
// replay thread handled present N-1. Calls that read or free GPU state that queued work may use drain the queue first
// (replay_drain), each cause counted.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "flight.h"
#include "gfx.h"
#include "host.h"
#include "nvn_api.h"
#include "vk.h"
#include "vk_exec.h"
#include "platform/platform.h"

#define WEAK __attribute__((weak))

typedef struct { VkImage image; VkFormat format; int w, h, has_crop, crop[4]; uint64_t frame, at, queued, value; } PresentJob;

static PlatMutex pl = PLAT_MUTEX_INIT;
static PlatCond pcv = PLAT_COND_INIT;   // job queued, job taken, job submitted
static PresentJob job;
static int pending, busy, present_thread;

// Sleeps on a high-resolution timer to 1 ms before at, then yields until at: the timer alone wakes up to ~0.5 ms late.
static void wait_until(uint64_t at) {
    uint64_t now = kern_now_ns();
    if (at > now + 1500000) plat_wait_ns(at - now - 1000000);
    while (kern_now_ns() < at) plat_yield();
}

// Waits until at like wait_until, watching the timeline: how long before at the GPU finished value (the frame's last
// blit), to the 0.5 ms; -1 when it had not by then.
static int64_t ready_slack(uint64_t at, uint64_t value) {
    uint64_t ready = 0;
    for (;;) {
        uint64_t now = kern_now_ns();
        if (!ready && vk_sync_fence_done() >= value) ready = now;
        if (now >= at) break;
        if (at - now > 1000000) plat_wait_ns(500000);
        else plat_yield();
    }
    return ready ? (int64_t)(at - ready) : -1;
}

static void present_main(void *arg) {
    (void)arg;
    uint64_t last = 0, last_queued = 0;
    plat_mutex_lock(&pl);
    for (;;) {
        while (!pending) plat_cond_wait(&pcv, &pl);
        PresentJob j = job;
        pending = 0;
        busy = 1;
        plat_cond_broadcast(&pcv);
        plat_mutex_unlock(&pl);
        if (flight_on) vk_thumbs_frame(j.frame);
        // Frame generation (vk_framegen.c): the made frames and the game's, all blitted now, each presented below at
        // its time; the blits read only frame generation's own images, written in queue order before them
        VkImage fo[VK_FG_MAX];
        int fw = 0, fh = 0;
        uint64_t step = 0;
        int nf = vk_fg_frame(j.image, j.w, j.h, j.has_crop ? j.crop : NULL, j.at, fo, &fw, &fh, &step);
        for (int k = 0; k < nf; k++) vk_present_blit_ahead(fo[k], VK_IMAGE_LAYOUT_GENERAL, VK_FORMAT_R8G8B8A8_UNORM, fw, fh, NULL);
        if (!nf) vk_present_blit(j.image, VK_IMAGE_LAYOUT_GENERAL, j.format, j.w, j.h, j.has_crop ? j.crop : NULL);
        uint64_t f = vk_sync_submit(NULL, 0);   // a timeline value after the blit: deferred destruction waits for the blit too
        if (j.value && f != j.value)
            rt_fatal("vk: the present of frame %llu signalled %llu, queued as %llu", (unsigned long long)j.frame,
                     (unsigned long long)f, (unsigned long long)j.value);
        plat_mutex_lock(&pl);
        busy = 0;
        plat_cond_broadcast(&pcv);
        plat_mutex_unlock(&pl);
        uint64_t t0 = kern_now_ns();
        // with frame generation the first frame at the display time, or now when that has passed, the next ones one
        // step apart: the game's frame goes out last
        uint64_t first = nf && t0 > j.at ? t0 : j.at;
        frame_jit_slack(j.at, ready_slack(nf ? first : j.at, f));   // MKW_LOW_LATENCY (core/boot/frame_jit.c)
        for (int k = 1; k < nf; k++) {
            wait_until(first + (uint64_t)(k - 1) * step);
            vk_lat_flip(j.at, k == 1, 0, vk_present_flip());   // MKW_LATENCY: the first made frame
        }
        uint64_t target = nf ? first + (uint64_t)(nf - 1) * step : j.at;
        wait_until(target);
        uint64_t t1 = kern_now_ns();
        vk_lat_flip(j.at, nf <= 1, 1, vk_present_flip());
        // MKW_TRACE=present: one line per present: the host present's interval, the interval of the game's present
        // calls (when the host present went out before back-edge pacing), the wait and how far past its target it went;
        // with frame generation, the game's frame and how many were shown before it
        if (rt_trace & RT_TRACE_PRESENT)
            rt_log("present: frame %llu interval %.3f ms, game %.3f ms, waited %.3f ms, %.3f ms past target, %d made",
                   (unsigned long long)j.frame, last ? (t1 - last) / 1e6 : 0.0,
                   last_queued ? (j.queued - last_queued) / 1e6 : 0.0, (t1 - t0) / 1e6,
                   target && t1 > target ? (t1 - target) / 1e6 : 0.0, nf ? nf - 1 : 0);
        last = t1;
        last_queued = j.queued;
        vk_pipeline_autosave();   // starts the autosave thread once
        plat_mutex_lock(&pl);
    }
    return;
}

void vk_present_drain(void) {
    uint64_t p0 = perf_begin();
    plat_mutex_lock(&pl);
    while (pending || busy) plat_cond_wait(&pcv, &pl);
    plat_mutex_unlock(&pl);
    perf_end(PERF_DRAIN, p0);
}

// The present after the frame count: resolves the image and hands the blit to the present thread. queued: when the game
// presented; value: the timeline value queued for this present (asynchronous replay), 0 for the next one (synchronous).
static void present_image(const tex_t *t, const int crop[4], uint64_t at_ns, uint64_t queued, uint64_t frame,
                          uint64_t value) {
    static volatile int32_t n_skipped;
    if (vk_pt_on()) vk_pt_frame(frame);
    vk_spec_frame(frame);
    vk_exec_present_note(frame);   // before vk_exec_frame_log, which resets the frame's draw counts
    vk_exec_frame_log(t, frame);
    vk_mem_census(frame);
    if (vk_dump_armed) vk_dump_present(frame);
    __atomic_store_n(&gfx_counters.validation_errors, (uint64_t)vk_validation_errors_total(), __ATOMIC_RELAXED);
    const VkTexImage *img = vk_texture_get(t);
    if (!img || !(img->aspect & VK_IMAGE_ASPECT_COLOR_BIT)) {
        if (plat_inc32(&n_skipped) <= 8)
            rt_log("vk: present of texture %p (format %#x) skipped: no colour image", (const void *)t, t ? t->d.format : 0);
        if (value) {   // the queued value is still signalled, in order
            vk_present_drain();
            uint64_t f = vk_sync_submit(NULL, 0);
            if (f != value) rt_fatal("vk: skipped present signalled %llu, queued as %llu", (unsigned long long)f, (unsigned long long)value);
        }
        return;
    }
    // The window's format says whether the game draws HDR (RGB10A2 0x3d or RGBA16F 0x29 instead of RGBA8 0x25 when
    // nn::vi says HDR10, gfx_hdr.c): logged at each change
    static uint32_t last_fmt;
    if (t->d.format != __atomic_exchange_n(&last_fmt, t->d.format, __ATOMIC_RELAXED))
        rt_log("vk: present of NVN format %#x (Vulkan %d) from frame %llu", t->d.format, (int)img->format,
               (unsigned long long)frame);
    PresentJob j = {.image = img->image, .format = img->format, .w = (int)img->width, .h = (int)img->height, .frame = frame,
                    .at = at_ns, .queued = queued, .value = value};
    if (crop) {
        j.has_crop = 1;
        for (int i = 0; i < 4; i++) j.crop[i] = crop[i];
    }
    vk_upload_flush(0);   // an upload vk_texture_get just recorded goes before the blit
    plat_mutex_lock(&pl);
    if (!present_thread) {
        if (!plat_thread_spawn(present_main, NULL, "vk present", 0)) rt_fatal("vk: cannot start the present thread");
        present_thread = 1;
    }
    while (pending) plat_cond_wait(&pcv, &pl);   // one present in waiting at most
    job = j;
    pending = 1;
    plat_cond_broadcast(&pcv);
    plat_mutex_unlock(&pl);
    if (rdoc_active()) { vk_present_drain(); rdoc_after_present(frame); }
}

// ---- Asynchronous replay
enum { RI_SUBMIT, RI_PRESENT, RI_RELEASE, RI_TEXREG, RI_SAMPREG };
// A held submission (submit_held): its handles once held_fill gave them (ready, under rl). The replay thread frees it.
typedef struct { int ready, n; uint64_t *h, submit_no; } Held;
typedef struct {
    int kind, n, has_crop, crop[4];
    uint64_t value, submit_no, at, queued;
    uint64_t *h;
    Held *held;         // RI_SUBMIT of a held submission: its handles come in there
    const tex_t *tex;   // RI_RELEASE, RI_TEXREG: a copy the item owns (the game may reuse the object's memory)
    void *reg;          // RI_TEXREG: the view (NULL none) and RI_SAMPREG: the sampler state, copies the item owns
    int id;             // RI_TEXREG, RI_SAMPREG: the slot
    uint32_t reg_at;    // RI_SUBMIT with MKW_REG_LATE: the texture registrations made before it (vk_tex_reg_limit)
} ReplayItem;
#define RQ_CAP 8192   // a frame queues 1 submit and 1 present, up to a few hundred releases and, with
                     // MKW_REG_QUEUED, its texture and sampler registrations
enum { DR_POOL_FINI, DR_MAP_VIRTUAL, DR_TEX_RELEASE, DR_PROG_RELEASE, DR_FINISH, DR_READ_TEXELS, DR_WRITE_TEXELS,
       DR_GUEST_READ, DR_FRAME, DR_FULL, DR_CHECK, DR_N };
static const char *const dr_name[DR_N] = {
    "pool_fini", "map_virtual", "texture_release (queued)", "program_release", "finish", "read_texels (writer fence)",
    "write_texels", "guest_read (writer fence)", "frame bound", "queue full", "check drain"};

static PlatMutex rl = PLAT_MUTEX_INIT;
static PlatCond rcv = PLAT_COND_INIT;   // item queued, item taken, item done
static ReplayItem rq[RQ_CAP];
static int rq_head, rq_n, r_busy, r_started;
static uint64_t r_presents_queued, r_presents_done, r_submit_done, game_frames;
// Under rl: calls, calls that waited and the time waited per cause, since the last replay_log line and over the run.
static uint64_t dr_calls[DR_N], dr_waits[DR_N], dr_ns[DR_N], dr_tcalls[DR_N], dr_twaits[DR_N], dr_tns[DR_N];
// MKW_REPLAY_CHECK: ReadTexels compared with a read after a full drain (under rl): checked, differing, differing
// although no submission recorded a write to the texture in between, differing with the queue idle at the first read.
static uint64_t ck_reads, ck_diff, ck_diff_unexplained, ck_diff_idle;

int vk_replay_async(void) {
    static int on = -1;
    if (on < 0) {
        on = cfg_bool("MKW_REPLAY_ASYNC");   // on by default: race 40.0 -> 32.1 and 45.4 -> 35.3 ms (rt-t*), one frame of latency more
        rt_log("vk: replay %s (MKW_REPLAY_ASYNC=%d)",
               on ? "on its own thread, at most one frame behind the game" : "on the submitting thread", on);
    }
    return on;
}

// The front end's view (nvn_api_core.c, nvn_api_cmd.c): replay is asynchronous only on this back end.
// Not the null back end: the Vulkan one, wrapped or not (runtime/gfx/cap: a capture must not change how the run replays).
int gfx_replay_async(void) { return gfx_backend() != &gfx_null_backend && vk_replay_async(); }

// The newest submit number (nvn_submit_count) whose replay is done: recording copies it used may be freed.
uint64_t gfx_replayed_submits(void) {
    if (!gfx_replay_async()) return nvn_submit_count();
    plat_mutex_lock_shared(&rl);
    uint64_t v = r_submit_done;
    plat_mutex_unlock_shared(&rl);
    return v;
}

static uint64_t submit_sync(const uint64_t *handles, int n) {
    vk_sync_start();
    vk_present_drain();
    vk_texture_new_submit();
    return vk_sync_submit(handles, n);
}

// The replay reached a held submission: its handles come when the other queue's submit that its waits need does
// (nvn_qorder.c). After MKW_QUEUE_HOLD_MS (3: Hookshot's first) without them the front end lets it go as it stands.
// Replay thread, rl not held.
static uint64_t hd_items, hd_waited, hd_forced, hd_ns, hd_max_ns, hd_presents, hd_drains;
static void held_wait(Held *h) {
    static int hold_ms = -1;
    if (hold_ms < 0) hold_ms = cfg_get("MKW_QUEUE_HOLD_MS") ? atoi(cfg_get("MKW_QUEUE_HOLD_MS")) : 3;
    plat_mutex_lock(&rl);
    uint64_t t0 = kern_now_ns(), d = 0;
    hd_items++;
    if (!h->ready) hd_waited++;
    while (!h->ready) {
        d = kern_now_ns() - t0;
        uint64_t limit = (uint64_t)hold_ms * 1000000u;
        if (d < limit) {
            plat_cond_wait_ns(&rcv, &rl, limit - d);
            continue;
        }
        hd_forced++;
        plat_mutex_unlock(&rl);
        gfx_held_force();
        plat_mutex_lock(&rl);
    }
    d = kern_now_ns() - t0;
    hd_ns += d;
    if (d > hd_max_ns) hd_max_ns = d;
    plat_mutex_unlock(&rl);
}

// MKW_REPLAY_CORE (default 1 with 6 physical cores or more and SMT, else 0; "ab" switches it with the A/B blocks): the
// replay thread alone on the last physical core (one of its logical processors, the sibling left idle) and every other
// thread off that core. Hookshot measured the replay ~1.7x slower sharing a core with the game's threads; the
// replay's recording is on the race frame's critical path (frame-carrera). rc-ab1..4
// (6 cores with SMT, Radeon 660M, 08/11/2026): race frame -1.17 ms (means), -1.03 (medians) with the scene balanced.
// wrc1/wrc1i (i5-8400, 6 cores without SMT, GTX 1050 Ti): +4.8 ms, a whole core taken from the game's threads. Fewer
// cores were not measured. The core is the last of the fastest ones (plat_cpu_fast): the last core alone was an
// efficiency core on a hybrid CPU, processor 27 of an i7-14700F at 4.2 GHz against 5.4, and the race frame took
// +4.0 ms (means, +5.1 medians, scene balanced: rc1/rc1i, RX 6700 XT, 16/11/2026) against the replay thread left
// unpinned. The others are every processor but those of that core (plat_cpu_core_of): taking the lower-numbered ones
// assumed SMT siblings numbered together, which AMD's are not on Linux (0 and 16 on 16 cores), and left the game's
// threads half the processors there. Applied at each present, on the replay thread.
static void replay_core(void) {
    static int mode = -1, applied = 0;
    static uint64_t all, own, others;
    if (mode < 0) {
        uint64_t first;
        int cores = plat_cpu_cores(&all, &first);
        mode = rt_knob("REPLAY_CORE", cores >= 6 && __builtin_popcountll(all) > cores ? "1" : "0");
        if (mode && cores >= 2) {
            uint64_t fast = plat_cpu_fast() & first, pick = fast ? fast : first;
            int c = 63 - __builtin_clzll(pick);
            own = 1ull << c, others = all & ~plat_cpu_core_of(c);
            rt_log("vk: the replay thread alone on processor %d (the last of %d %s cores), the others on %#llx%s "
                   "(MKW_REPLAY_CORE)", c, __builtin_popcountll(pick), pick == first ? "physical" : "fastest",
                   (unsigned long long)others, mode == 2 ? " in the B blocks" : "");
        } else {
            mode = 0;
        }
    }
    int on = RT_KNOB_ON(mode);
    if (on == applied) return;
    applied = on;
    plat_process_affinity(on ? others : all);
    plat_thread_affinity(NULL, on ? own : all);
}

// MKW_REPLAY_DELAY_US (a measurement, default 0): the replay thread waits that long before each
// submission, so a capture's replay (--replay with MKW_REPLAY_ASYNC=1) restores the memory the game wrote after a submit
// while that submit is still to be recorded, as the game's threads do in a run.
static void replay_main(void *arg) {
    (void)arg;
    perf_replay_thread();
    const char *de = cfg_get("MKW_REPLAY_DELAY_US");
    uint64_t delay_ns = de ? strtoull(de, NULL, 10) * 1000 : 0;
    if (delay_ns) rt_log("vk: the replay waits %llu us before each submission (MKW_REPLAY_DELAY_US)", (unsigned long long)(delay_ns / 1000));
    plat_mutex_lock(&rl);
    for (;;) {
        int64_t w0 = vk_tl_on() > 0 ? (int64_t)plat_now_ns() : 0;
        while (!rq_n) plat_cond_wait(&rcv, &rl);
        int64_t w1 = w0 ? (int64_t)plat_now_ns() : 0;
        if (w0) vk_tl_replay_time(VK_TL_IDLE, w1 - w0);
        ReplayItem it = rq[rq_head];
        rq_head = (rq_head + 1) % RQ_CAP;
        rq_n--;
        r_busy = 1;
        plat_cond_broadcast(&rcv);
        plat_mutex_unlock(&rl);
        if (it.kind == RI_SUBMIT && it.held) {
            held_wait(it.held);
            it.h = it.held->h, it.n = it.held->n;
            if (it.held->submit_no > it.submit_no) it.submit_no = it.held->submit_no;   // it holds work submitted since
            free(it.held);
        }
        if (it.kind == RI_SUBMIT) {
            if (delay_ns) plat_wait_ns(delay_ns);
            __atomic_store_n(&vk_replay_submit, it.submit_no, __ATOMIC_RELAXED);   // shadow syncs apply captures up to it
            vk_tl_replay(it.value, 0);
            vk_tex_reg_limit = it.reg_at;
            uint64_t f = submit_sync(it.h, it.n);
            vk_tex_reg_limit = 0;
            vk_tl_replay(it.value, 1);
            if (f != it.value)
                rt_fatal("vk: replayed submission signalled %llu, queued as %llu", (unsigned long long)f,
                         (unsigned long long)it.value);
            free(it.h);
        } else if (it.kind == RI_RELEASE) {
            vk_present_drain();
            vk_texture_release(it.tex);
            free((void *)it.tex);
        } else if (it.kind == RI_TEXREG) {
            vk_texture_register(it.id, it.tex, it.reg);
            free((void *)it.tex), free(it.reg);
        } else if (it.kind == RI_SAMPREG) {
            vk_sampler_register(it.id, it.reg);
            free(it.reg);
        } else {
            replay_core();
            uint64_t frame = __atomic_add_fetch(&gfx_counters.frames, 1, __ATOMIC_RELAXED);
            present_image(it.tex, it.has_crop ? it.crop : NULL, it.at, it.queued, frame, it.value);
            vk_tl_present(it.value, frame);
        }
        if (w0) vk_tl_replay_time(it.kind == RI_SUBMIT ? VK_TL_SUBMIT : it.kind == RI_RELEASE ? VK_TL_RELEASE
                                  : it.kind == RI_PRESENT ? VK_TL_PRESENT : VK_TL_REG, (int64_t)plat_now_ns() - w1);
        plat_mutex_lock(&rl);
        // Only presents count: a release counted as one let the frame bound (vk_be_present) pass while the replay was
        // still a frame or more behind (a frame queues up to a few hundred releases).
        if (it.kind == RI_SUBMIT) r_submit_done = it.submit_no;
        else if (it.kind == RI_PRESENT) r_presents_done++;
        r_busy = 0;
        plat_cond_broadcast(&rcv);
    }
    return;
}

static void wait_counted(int why, uint64_t t0) {   // rl held
    uint64_t d = kern_now_ns() - t0;
    dr_waits[why]++, dr_twaits[why]++;
    dr_ns[why] += d, dr_tns[why] += d;
    perf_end(PERF_DRAIN, t0);
}

// A guest thread waiting for the replay gives its virtual core up, as around any blocking host wait (NVN_BLOCKING):
// guest_read (memcpy, pool invalidate), pool and texture finalize run without it, and a drain lasts up to a replay.
// Called and returning with rl held; rl is not held while the core is taken back.
static int core_away_unlocked(void) {
    plat_mutex_unlock(&rl);
    int away = kern_core_away();
    plat_mutex_lock(&rl);
    return away;
}
static void core_back_unlocked(int *away) {
    if (!*away) return;
    plat_mutex_unlock(&rl);
    kern_core_back(away);
    plat_mutex_lock(&rl);
}

// Queues an item (waits while the queue is full) and returns its value, reserved under rl so values follow queue order.
static int reg_late(void);
static uint64_t replay_enqueue(ReplayItem *it) {
    plat_mutex_lock(&rl);
    if (!r_started) {
        if (!plat_thread_spawn(replay_main, NULL, "vk replay", 0)) rt_fatal("vk: cannot start the replay thread");
        r_started = 1;
    }
    // rl is dropped while the core is taken back, and another thread may fill the queue again meanwhile: check again.
    while (rq_n == RQ_CAP) {
        uint64_t p0 = perf_begin();
        dr_calls[DR_FULL]++, dr_tcalls[DR_FULL]++;
        int away = core_away_unlocked();
        while (rq_n == RQ_CAP) plat_cond_wait(&rcv, &rl);
        wait_counted(DR_FULL, p0);
        core_back_unlocked(&away);
    }
    it->value = it->kind == RI_SUBMIT || it->kind == RI_PRESENT ? vk_sync_reserve() : 0;   // the others signal nothing
    if (it->kind == RI_SUBMIT && reg_late()) it->reg_at = vk_texture_reg_serial();
    if (it->kind == RI_SUBMIT) vk_tl_sub(it->value);   // before the replay thread can take it
    rq[(rq_head + rq_n) % RQ_CAP] = *it;
    rq_n++;
    plat_cond_broadcast(&rcv);
    plat_mutex_unlock(&rl);
    return it->value;
}

// Returns once every queued item was replayed and submitted. Back-end entries only: the replay thread never calls them.
static void replay_drain(int why) {
    if (!gfx_replay_async()) return;
    int let_go = gfx_held_force();   // (held work waits for a submit that may come only after this returns)
    plat_mutex_lock(&rl);
    hd_drains += (uint64_t)let_go;
    dr_calls[why]++, dr_tcalls[why]++;
    if (rq_n || r_busy) {
        uint64_t p0 = perf_begin();
        int away = core_away_unlocked();
        while (rq_n || r_busy) plat_cond_wait(&rcv, &rl);
        wait_counted(why, p0);
        core_back_unlocked(&away);
    }
    plat_mutex_unlock(&rl);
}

// What made the game wait for the replay: since the last line (every 1800 game frames), or over the run (at exit).
static void replay_log(uint64_t frame, int total) {
    char buf[1024];
    int k = 0;
    plat_mutex_lock(&rl);
    const uint64_t *c = total ? dr_tcalls : dr_calls, *w = total ? dr_twaits : dr_waits, *t = total ? dr_tns : dr_ns;
    for (int i = 0; i < DR_N && k < (int)sizeof buf - 96; i++)
        if (c[i] || w[i])
            k += snprintf(buf + k, sizeof buf - (size_t)k, "%s %s %llu calls, %llu waited %.1f ms", k ? ";" : "", dr_name[i],
                          (unsigned long long)c[i], (unsigned long long)w[i], t[i] / 1e6);
    if (ck_reads && k < (int)sizeof buf - 128)
        k += snprintf(buf + k, sizeof buf - (size_t)k, "; check (run): %llu reads, %llu differ, %llu without a recorded write, %llu with the queue idle",
                      (unsigned long long)ck_reads, (unsigned long long)ck_diff, (unsigned long long)ck_diff_unexplained,
                      (unsigned long long)ck_diff_idle);
    if (hd_items && k < (int)sizeof buf - 200)   // (run totals)
        k += snprintf(buf + k, sizeof buf - (size_t)k, "; held submissions (run): %llu, %llu reached empty, %llu let go "
                      "after MKW_QUEUE_HOLD_MS, waited %.1f ms (%.2f at most); waits let go at presents %llu, at drains "
                      "%llu", (unsigned long long)hd_items, (unsigned long long)hd_waited, (unsigned long long)hd_forced,
                      hd_ns / 1e6, hd_max_ns / 1e6, (unsigned long long)hd_presents, (unsigned long long)hd_drains);
    memset(dr_calls, 0, sizeof dr_calls);
    memset(dr_waits, 0, sizeof dr_waits);
    memset(dr_ns, 0, sizeof dr_ns);
    plat_mutex_unlock(&rl);
    rt_log("vk: replay queue %s frame %llu:%s", total ? "over the run to" : "at", (unsigned long long)frame,
           k ? buf : " no waits");
}

// The run total, once: the game exits through prof_write (boot.c _exit, the window close), probes through
// vk_backend_shutdown.
void gfx_exit_log(void) {
    static int32_t done;
    if (gfx_replay_async() && r_started && !plat_xchg32(&done, 1)) replay_log(game_frames, 1);
}

static void vk_be_present(const tex_t *t, const int crop[4], uint64_t at_ns) {
    vk_sync_start();
    if (vk_glyphs_due()) {   // the button icons changed: their sheet goes up again, as a WriteTexels would
        replay_drain(DR_WRITE_TEXELS);
        vk_present_drain();
        vk_glyphs_redo();
    }
    if (!gfx_replay_async()) {
        uint64_t frame = __atomic_add_fetch(&gfx_counters.frames, 1, __ATOMIC_RELAXED);
        perf_present(frame);   // before vk_exec_frame_log, which resets the frame's exec counts the flight recorder reads
        present_image(t, crop, at_ns, kern_now_ns(), frame, 0);
        return;
    }
    // Held work goes before the present as it stands (nvn_qorder.c): on the console the present is the main queue's and
    // does not wait for a worker queue stopped at a wait (one stopped at the end of a frame is let on only by the next
    // frame's first submit); behind it on our one queue, the frame would show the next one's work begun.
    int let_go = gfx_held_force();
    // Frame bound: the replay thread handled the previous present before this one is queued.
    plat_mutex_lock(&rl);
    hd_presents += (uint64_t)let_go;
    dr_calls[DR_FRAME]++, dr_tcalls[DR_FRAME]++;
    if (r_presents_done < r_presents_queued) {
        uint64_t p0 = perf_begin();
        while (r_presents_done < r_presents_queued) plat_cond_wait(&rcv, &rl);
        wait_counted(DR_FRAME, p0);
    }
    r_presents_queued++;
    plat_mutex_unlock(&rl);
    uint64_t frame = ++game_frames;
    perf_present(frame);
    if (frame % 1800 == 0) replay_log(frame, 0);
    ReplayItem it = {.kind = RI_PRESENT, .tex = t, .at = at_ns, .queued = kern_now_ns()};
    if (crop) {
        it.has_crop = 1;
        for (int i = 0; i < 4; i++) it.crop[i] = crop[i];
    }
    replay_enqueue(&it);
}

// vk_pipeline.c drops a released program's pipelines here.
WEAK void vk_program_release(const prog_t *p) { (void)p; }

static int vk_be_pool_init(pool_t *p) { vk_sync_start(); return vk_pool_init(p); }
// Only a pool whose GPU storage is guest memory drains the replay queue: the guest gets that storage back on return. The
// others leave the index now and their storage goes once the GPU is done with what was submitted (vk_pool_fini); the game frees
// a pool only after waiting for the GPU's last use of it. Draining cost ~24 ms per finalize while loading (live35).
static void vk_be_pool_fini(pool_t *p) {
    vk_sync_start();
    if (vk_pool_needs_drain(p)) replay_drain(DR_POOL_FINI);
    else if (gfx_replay_async()) {
        plat_mutex_lock(&rl);
        dr_calls[DR_POOL_FINI]++, dr_tcalls[DR_POOL_FINI]++;
        plat_mutex_unlock(&rl);
    }
    vk_present_drain();
    vk_pool_fini(p);
}
// MapVirtual changes the page table at the call, as NVN does on the CPU timeline; the game remaps pages only once the
// GPU is done with them. A drain made the loading wait for the queued replays, 94 times and 2.4 s over a course load
// (rt-u1). MKW_MAPV_DRAIN=1 drains as before (the queued replays then resolve with the old mapping).
static void vk_be_mapv_drain(void) {
    static int on = -1;
    if (on < 0) on = rt_knob("MAPV_DRAIN", "0");
    if (RT_KNOB_ON(on)) replay_drain(DR_MAP_VIRTUAL);
    else if (gfx_replay_async()) {
        plat_mutex_lock(&rl);
        dr_calls[DR_MAP_VIRTUAL]++, dr_tcalls[DR_MAP_VIRTUAL]++;
        plat_mutex_unlock(&rl);
    }
}
static int vk_be_pool_map_virtual(pool_t *v, int n, const void *requests) {
    vk_sync_start();
    vk_be_mapv_drain();
    vk_present_drain();
    return vk_pool_map_virtual(v, n, requests);
}
// MKW_REG_QUEUED (default 0; a measurement until the flicker it targets is shown gone): texture and sampler slots
// registered in queue order, as releases are. Applied at once, a replay a few submits behind the game resolved a slot
// to what the game registered there for a later frame: a target not rendered yet has no image and binds a stand-in
// (the title's shadow map did the same between chunks, grabacion-paralela).
static int reg_queued(void) {
    static int on = -1;
    if (on < 0) on = cfg_get("MKW_REG_QUEUED") && atoi(cfg_get("MKW_REG_QUEUED"));
    return on && gfx_replay_async();
}
// MKW_REG_LATE=1 (a measurement): with the registrations applied at once, which lookups of the replay see one made after
// their submission (vk_tex_slots.c late_note)
static int reg_late(void) {
    static int on = -1;
    if (on < 0) {
        on = cfg_get("MKW_REG_LATE") && atoi(cfg_get("MKW_REG_LATE")) > 0 && !reg_queued() && gfx_replay_async();
        if (on) rt_log("vk: slot lookups of a registration made after their submission logged per frame (MKW_REG_LATE)");
    }
    return on;
}
static void vk_be_texture_register(int id, const tex_t *t, const view_t *v) {
    vk_sync_start();
    if (!reg_queued()) {
        vk_texture_register(id, t, v);
        return;
    }
    tex_t *tc = malloc(sizeof *tc);
    view_t *vc = v ? malloc(sizeof *vc) : NULL;
    if (!tc || (v && !vc)) rt_fatal("vk: out of memory (replay queue)");
    *tc = *t;
    if (v) *vc = *v;
    ReplayItem it = {.kind = RI_TEXREG, .tex = tc, .reg = vc, .id = id};
    replay_enqueue(&it);
}
static void vk_be_sampler_register(int id, const samp_par *s) {
    vk_sync_start();
    if (!reg_queued()) {
        vk_sampler_register(id, s);
        return;
    }
    samp_par *sc = malloc(sizeof *sc);
    if (!sc) rt_fatal("vk: out of memory (replay queue)");
    *sc = *s;
    ReplayItem it = {.kind = RI_SAMPREG, .reg = sc, .id = id};
    replay_enqueue(&it);
}
// Asynchronous: the release is queued behind the submissions before it, so their replay still resolves the texture's
// slots to its image (a released slot follows another live texture at the storage), and the ones after it see it
// released, the order of the synchronous replay. vk_texture_release reads only the object's key fields: a copy.
static void vk_be_texture_release(const tex_t *t) {
    vk_sync_start();
    if (!gfx_replay_async()) {
        vk_present_drain();
        vk_texture_release(t);
        return;
    }
    if (!t || !t->mem) return;
    tex_t *copy = malloc(sizeof *copy);
    if (!copy) rt_fatal("vk: out of memory (replay queue)");
    *copy = *t;
    ReplayItem it = {.kind = RI_RELEASE, .tex = copy};
    plat_mutex_lock(&rl);
    dr_calls[DR_TEX_RELEASE]++, dr_tcalls[DR_TEX_RELEASE]++;
    plat_mutex_unlock(&rl);
    replay_enqueue(&it);
}
static void vk_be_program_release(const prog_t *p) { replay_drain(DR_PROG_RELEASE); vk_program_release(p); }
// Asynchronous: the handles (nvn_submit_prepare_async ran under the front end's submit lock) are copied and queued;
// the value returned is the one the replay will signal.
static uint64_t vk_be_submit(const uint64_t *handles, int n) {
    if (!gfx_replay_async()) return submit_sync(handles, n);
    vk_sync_start();
    ReplayItem it = {.kind = RI_SUBMIT, .n = n, .submit_no = nvn_submit_count()};
    vk_mem_shadow_capture(it.submit_no);   // before the replay can sync for it
    vk_usnap_take(handles, n);              // MKW_USNAP_AT_SUBMIT: the uniform blocks as the submit finds them
    if (n > 0) {
        it.h = malloc(sizeof *it.h * (size_t)n);
        if (!it.h) rt_fatal("vk: out of memory (replay queue)");
        memcpy(it.h, handles, sizeof *it.h * (size_t)n);
    }
    return replay_enqueue(&it);
}
static uint64_t vk_be_submit_held(void **slot) {
    vk_sync_start();
    Held *h = calloc(1, sizeof *h);
    if (!h) rt_fatal("vk: out of memory (held submission)");
    *slot = h;
    ReplayItem it = {.kind = RI_SUBMIT, .submit_no = nvn_submit_count(), .held = h};
    vk_mem_shadow_capture(it.submit_no);
    return replay_enqueue(&it);
}
static void vk_be_held_fill(void *slot, const uint64_t *handles, int n) {
    Held *h = slot;
    uint64_t *c = n > 0 ? malloc(sizeof *c * (size_t)n) : NULL;
    if (n > 0 && !c) rt_fatal("vk: out of memory (replay queue)");
    if (n > 0) memcpy(c, handles, sizeof *c * (size_t)n);
    plat_mutex_lock(&rl);
    h->h = c, h->n = n, h->submit_no = nvn_submit_count(), h->ready = 1;
    plat_cond_broadcast(&rcv);
    plat_mutex_unlock(&rl);
}
static void vk_be_finish(void) { vk_sync_start(); replay_drain(DR_FINISH); vk_present_drain(); vk_sync_finish(); }
static int replay_check(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = cfg_get("MKW_REPLAY_CHECK");
        on = e && strcmp(e, "0");
        if (on) rt_log("vk: MKW_REPLAY_CHECK: every ReadTexels is read again after a full replay drain and compared");
    }
    return on;
}

// ReadTexels with the replay queued: waits for the newest submission recorded to write the texture (replayed and
// completed), not for the whole queue. The readback copy goes through the upload batch on the same queue, after every
// submission the replay has submitted. Submissions still queued or being recorded run after the copy, as on hardware
// when the GPU has not reached them yet: the game only sees their texels after waiting on a sync, and every sync
// value covers replay and GPU, so a read the game synchronised sees them. MKW_REPLAY_CHECK compares with a full drain.
static void vk_be_read_texels(const tex_t *t, const view_t *v, const NVNcopyRegion *r, uint8_t *dst, uint64_t row,
                              uint64_t img) {
    vk_sync_start();
    if (!gfx_replay_async()) {
        vk_read_texels(t, v, r, dst, row, img);
        return;
    }
    uint64_t f = vk_texture_last_write(t);
    plat_mutex_lock(&rl);
    dr_calls[DR_READ_TEXELS]++, dr_tcalls[DR_READ_TEXELS]++;
    int idle = !rq_n && !r_busy;
    plat_mutex_unlock(&rl);
    if (f && vk_sync_fence_done() < f) {
        uint64_t t0 = kern_now_ns();
        vk_sync_fence_wait(f, UINT64_MAX);   // counts as a fence wait in perf
        uint64_t d = kern_now_ns() - t0;
        plat_mutex_lock(&rl);
        dr_waits[DR_READ_TEXELS]++, dr_twaits[DR_READ_TEXELS]++;
        dr_ns[DR_READ_TEXELS] += d, dr_tns[DR_READ_TEXELS] += d;
        plat_mutex_unlock(&rl);
    }
    vk_read_texels(t, v, r, dst, row, img);
    if (!replay_check()) return;
    size_t n = vk_texture_texel_span(t, v, r, row, img);
    uint8_t *again = n ? malloc(n) : NULL;
    if (!again) return;
    memcpy(again, dst, n);   // bytes between the rows stay the caller's
    uint64_t f0 = vk_texture_last_write(t);
    replay_drain(DR_CHECK);
    uint64_t f1 = vk_texture_last_write(t);
    vk_read_texels(t, v, r, again, row, img);
    int differ = memcmp(again, dst, n) != 0;
    static volatile int32_t n_logged;
    if (differ && plat_inc32(&n_logged) <= 40) {
        size_t at = 0;
        while (at < n && again[at] == dst[at]) at++;
        rt_log("vk: replay check: ReadTexels of %p (format %#x, %dx%dx%d at %d,%d,%d) differs at byte %zu of %zu; writer "
               "fence %llu at the read, %llu after the drain, done %llu; queue %s at the read",
               (void *)t->mem, t->d.format, r->width, r->height, r->depth, r->xoffset, r->yoffset, r->zoffset, at, n,
               (unsigned long long)f0, (unsigned long long)f1, (unsigned long long)vk_sync_fence_done(), idle ? "idle" : "busy");
    }
    plat_mutex_lock(&rl);
    ck_reads++;
    if (differ) {
        ck_diff++;
        if (f1 == f0) ck_diff_unexplained++;
        if (idle) ck_diff_idle++;
    }
    plat_mutex_unlock(&rl);
    free(again);
}
static void vk_be_write_texels(const tex_t *t, const view_t *v, const NVNcopyRegion *r, const uint8_t *src, uint64_t row,
                               uint64_t img) {
    vk_sync_start();
    replay_drain(DR_WRITE_TEXELS);
    vk_present_drain();
    vk_write_texels(t, v, r, src, row, img);
}
static void vk_be_guest_written(uint64_t addr, uint64_t size) { vk_sync_start(); vk_guest_written(addr, size); }
// A CPU read of GPU-written mirrored storage (memcpy, pool invalidate). With the replay queued it waits for the newest
// submission recorded to write the textures there, not for the whole queue: the write-back copy then runs after their
// renders, and clears gpu_dirty only for content that is on the queue. Submissions not replayed yet render after the
// copy, as on hardware when the GPU has not reached them; they mark the texture dirty again for a later read.
static void vk_be_guest_read(uint64_t addr, uint64_t size) {
    if (!vk_texture_cpu_stale(addr, size)) return;
    vk_sync_start();
    if (gfx_replay_async()) {
        uint64_t f = vk_texture_dirty_fence(addr, size);
        plat_mutex_lock(&rl);
        dr_calls[DR_GUEST_READ]++, dr_tcalls[DR_GUEST_READ]++;
        plat_mutex_unlock(&rl);
        if (f && vk_sync_fence_done() < f) {
            uint64_t t0 = kern_now_ns();
            NVN_BLOCKING();
            vk_sync_fence_wait(f, UINT64_MAX);
            uint64_t d = kern_now_ns() - t0;
            plat_mutex_lock(&rl);
            dr_waits[DR_GUEST_READ]++, dr_twaits[DR_GUEST_READ]++;
            dr_ns[DR_GUEST_READ] += d, dr_tns[DR_GUEST_READ] += d;
            plat_mutex_unlock(&rl);
        }
    }
    vk_present_drain();
    vk_texture_write_back(addr, size);
}
// After a fence wait: the value is done, so its replay is too; a later submission the replay records meanwhile marks
// its targets with a later fence, which write_back_small leaves alone. No drain.
static void vk_be_cpu_synced(uint64_t fence) {
    if (!vk_texture_small_dirty()) return;
    vk_sync_start();
    vk_present_drain();
    vk_texture_write_back_small(fence);
}

const GfxBackend gfx_vulkan_backend = {
    .name = "vulkan",
    .pool_init = vk_be_pool_init,
    .pool_fini = vk_be_pool_fini,
    .pool_map_virtual = vk_be_pool_map_virtual,
    .texture_register = vk_be_texture_register,
    .sampler_register = vk_be_sampler_register,
    .texture_release = vk_be_texture_release,
    .program_release = vk_be_program_release,
    .submit = vk_be_submit,
    .submit_held = vk_be_submit_held,
    .held_fill = vk_be_held_fill,
    .fence_wait = vk_sync_fence_wait,
    .fence_done = vk_sync_fence_done,
    .fence_queued = vk_sync_fence_queued,
    .fence_wait_issued = vk_sync_issued_wait,
    .present = vk_be_present,
    .finish = vk_be_finish,
    .read_texels = vk_be_read_texels,
    .write_texels = vk_be_write_texels,
    .guest_written = vk_be_guest_written,
    .guest_read = vk_be_guest_read,
    .cpu_synced = vk_be_cpu_synced,
};

void vk_backend_shutdown(void) {
    replay_drain(DR_FINISH);
    gfx_exit_log();
    vk_present_drain();
    vk_sync_shutdown();
    vk_rtstats_shutdown();
    vk_dump_shutdown();
    vk_mem_shutdown();
    vk_shutdown();
}
