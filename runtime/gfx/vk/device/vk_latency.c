// MKW_LATENCY=1 (diagnosis, off by default): how long a controller reading takes to reach the screen. Per game frame:
//   read   the game's first controller read since its last present (input_poll, the game thread: lat_input_read);
//   call   its nvnQueuePresentTexture, and the pacer's display time for it (slot);
//   flip   the host present (vkQueuePresentKHR) of the first picture with the frame's content (with frame generation
//          the first made frame, a blend with the previous frame) and of the game's own picture;
//   shown  when each of those reached the screen: vkWaitForPresentKHR on a thread of its own (VK_KHR_present_id and
//          VK_KHR_present_wait, enabled only with MKW_LATENCY); without them the line stops at the flips.
// Every 5 s a "latency:" line gives the medians from the read. A press waits for the next read: on average half a
// frame more (half the median read interval, the "press" column), as on the console. The game's own delay between
// reading the controller and drawing the result (whether it draws this frame's state or the last) is not counted: it
// is the console's too.
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx_wsys.h"
#include "host.h"
#include "platform/platform.h"
#include "vk.h"

#define RING 32           // game frames kept: at 15 fps, two seconds
#define IDS 64            // presents waiting for the screen
#define WINDOW_NS 5000000000ull
#define MAX_SAMPLES 512
#define WAIT_SLICE_NS 5000000ull   // each wait gives the swapchain lock back this often (a resize waits at most this)

typedef struct { uint64_t slot, read, call, flip[2], id[2], shown[2]; } LatFrame;   // [0] first picture, [1] game's
enum { S_PRESS, S_CALL, S_SLOT, S_FLIP0, S_FLIP1, S_SHOWN0, S_SHOWN1, S_N };
static const char *const s_name[S_N] = {"press", "call", "slot", "flip first", "flip game", "screen first", "screen game"};

static int on = -1, waits;   // waits: present wait available and enabled
static PlatMutex lk = PLAT_MUTEX_INIT;
static LatFrame ring[RING];
static unsigned n_frames;
static _Atomic uint64_t pending_read, last_read_gap;
static uint64_t last_read;
static float samples[S_N][MAX_SAMPLES];
static unsigned n_samples[S_N];
static uint64_t window_t0;
static PFN_vkWaitForPresentKHR wait_fn;
static VkDevice dev;
static PlatMutex sc_lock = PLAT_MUTEX_INIT;   // the swapchain the waiter waits on: not retired while it waits
static VkSwapchainKHR sc_now;
static uint64_t id_next, id_q[IDS];
static unsigned id_head, id_tail;
static PlatMutex id_lock = PLAT_MUTEX_INIT;
static PlatCond id_cv = PLAT_COND_INIT;

int vk_lat_on(void) {
    if (on < 0) on = cfg_get("MKW_LATENCY") && atoi(cfg_get("MKW_LATENCY")) > 0;
    return on;
}

static _Atomic uint64_t last_call;   // the game's last nvnQueuePresentTexture

// MKW_LATENCY with MKW_PROBE's window (seqwatch.c): one line per event of the frame (the frame update, the acquire and
// its wait, the controller reads, the present) with its thread and time, to see which frame a read reaches.
void lat_mark(const char *what) {
    if (vk_lat_on() && seqwatch_probe())
        rt_log("latency: %s on thread %u, %.2f ms after the game's last present", what, plat_thread_id(),
               (kern_now_ns() - atomic_load(&last_call)) / 1e6);
}

// Every controller read, with MKW_PROBE's window (seqwatch.c): which thread reads and when in the frame, so a read is
// paired with the frame that uses it.
void lat_input_read(void) {
    if (!vk_lat_on()) return;
    uint64_t now = kern_now_ns(), zero = 0;
    lat_mark("controller read");
    if (atomic_compare_exchange_strong(&pending_read, &zero, now)) {
        if (last_read) atomic_store(&last_read_gap, now - last_read);
        last_read = now;
    }
}

static void sample(int s, uint64_t from, uint64_t to) {
    if (from && to >= from && n_samples[s] < MAX_SAMPLES) samples[s][n_samples[s]++] = (float)((to - from) / 1e6);
}

static int cmp_float(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

static float median(int s) {
    if (!n_samples[s]) return -1.f;
    qsort(samples[s], n_samples[s], sizeof(float), cmp_float);
    return samples[s][n_samples[s] / 2];
}

// A frame is done when its game picture was shown (or flipped, without present wait): its deltas join the window.
static void frame_done(const LatFrame *f) {   // lk held
    if (!f->read) return;
    uint64_t gap = atomic_load(&last_read_gap);
    if (n_samples[S_PRESS] < MAX_SAMPLES && gap) samples[S_PRESS][n_samples[S_PRESS]++] = (float)(gap / 2e6);
    sample(S_CALL, f->read, f->call);
    sample(S_SLOT, f->read, f->slot);
    sample(S_FLIP0, f->read, f->flip[0]);
    sample(S_FLIP1, f->read, f->flip[1]);
    sample(S_SHOWN0, f->read, f->shown[0]);
    sample(S_SHOWN1, f->read, f->shown[1]);
    uint64_t now = kern_now_ns();
    if (!window_t0) window_t0 = now;
    if (now - window_t0 < WINDOW_NS) return;
    char line[512];
    int n = snprintf(line, sizeof line, "latency: from the game's read, median ms over %u frames:", n_samples[S_CALL]);
    for (int s = 0; s < S_N && n < (int)sizeof line; s++) {
        float m = median(s);
        if (m >= 0) n += snprintf(line + n, sizeof line - (size_t)n, " %s %.1f%s", s_name[s], m, s ? "" : " (mean wait)");
    }
    rt_log("%s%s", line, !waits ? " (no present wait: up to the flips)"
                         : wsys_minimized() ? " (window minimised: the screen times are not shown ones)" : "");
    memset(n_samples, 0, sizeof n_samples);
    window_t0 = now;
}

static LatFrame *find_slot(uint64_t slot) {   // lk held
    for (unsigned i = 0; i < RING; i++)
        if (ring[i].slot == slot) return &ring[i];
    return NULL;
}

// The read taken here is the next frame's: the game reads the controller as its logic starts a frame (main thread)
// and its render thread presents the frame before it 0.3 ms later (lat1, 15/11/2026), so a present carries the read
// taken at the previous one.
void lat_game_present(uint64_t slot) {
    if (!vk_lat_on()) return;
    static uint64_t carried;
    uint64_t now = kern_now_ns(), read = carried;
    carried = atomic_exchange(&pending_read, 0);
    lat_mark("game present");
    atomic_store(&last_call, now);
    plat_mutex_lock(&lk);
    ring[n_frames++ % RING] = (LatFrame){.slot = slot, .read = read, .call = now};
    plat_mutex_unlock(&lk);
}

void vk_lat_flip(uint64_t slot, int first, int game, uint64_t id) {
    if (!vk_lat_on()) return;
    uint64_t now = kern_now_ns();
    plat_mutex_lock(&lk);
    LatFrame *f = find_slot(slot);
    if (f) {
        for (int k = 0; k < 2; k++)
            if ((k ? game : first) && !f->flip[k]) f->flip[k] = now, f->id[k] = id;
        if (game && !waits) frame_done(f);
    }
    plat_mutex_unlock(&lk);
}

// The present id of the next host present (0: none), called under the present lock in queue order.
uint64_t vk_lat_next_id(void) {
    if (!waits) return 0;
    plat_mutex_lock(&id_lock);
    uint64_t id = ++id_next;
    if (id_tail - id_head < IDS) id_q[id_tail++ % IDS] = id;
    plat_cond_signal(&id_cv);
    plat_mutex_unlock(&id_lock);
    return id;
}

static void shown(uint64_t id, uint64_t t) {
    plat_mutex_lock(&lk);
    for (unsigned i = 0; i < RING; i++) {
        LatFrame *f = &ring[i];
        for (int k = 0; k < 2; k++)
            if (f->id[k] == id && !f->shown[k]) {
                f->shown[k] = t;
                if (k == 1) frame_done(f);
            }
    }
    plat_mutex_unlock(&lk);
}

// Every present's step to the next on the screen, in refreshes, counted per 5 s: 1 is a frame per refresh, 0 a present
// another replaced before it was shown (mailbox: present wait returns with the next one), 2+ a refresh that showed the
// same frame again. 60 shown with frame generation is not 60 even ones (fgm1, mailbox: at 20x3 and 15x4 6-18 % of
// the refreshes replaced or repeated; Adrià 15/11/2026: "a x4 ... se ve como lagueado").
static void steps_note(uint64_t t) {
    static uint64_t last, t0;
    static unsigned n[4];
    double hz = vk_present_refresh_hz();
    if (last && hz > 0) {
        long k = lround((double)(t - last) * hz / 1e9);
        n[k < 0 ? 0 : k > 3 ? 3 : k]++;
    }
    last = t;
    if (!t0) t0 = t;
    if (t - t0 < 5000000000ull) return;
    rt_log("latency: shown steps in refreshes over %.1f s: 0 (replaced) %u, 1 %u, 2 %u, 3+ %u", (t - t0) / 1e9, n[0],
           n[1], n[2], n[3]);
    memset(n, 0, sizeof n);
    t0 = t;
}

static void waiter_main(void *arg) {
    (void)arg;
    for (;;) {
        plat_mutex_lock(&id_lock);
        while (id_head == id_tail) plat_cond_wait(&id_cv, &id_lock);
        uint64_t id = id_q[id_head % IDS];
        plat_mutex_unlock(&id_lock);
        VkResult r = VK_TIMEOUT;
        for (int tries = 0; r == VK_TIMEOUT && tries < 40; tries++) {   // 200 ms, then the id is given up
            plat_mutex_lock(&sc_lock);
            r = sc_now ? wait_fn(dev, sc_now, id, WAIT_SLICE_NS) : VK_ERROR_OUT_OF_DATE_KHR;
            plat_mutex_unlock(&sc_lock);
        }
        if (r == VK_SUCCESS) {
            uint64_t t = kern_now_ns();
            shown(id, t);
            steps_note(t);
        }
        plat_mutex_lock(&id_lock);
        id_head++;
        plat_mutex_unlock(&id_lock);
    }
}

// gfx_present.c, around vkCreateSwapchainKHR (it retires the old one): begin(1) before, begin(0) after with the new one.
void vk_lat_swapchain(int begin, VkSwapchainKHR sc) {
    if (!waits) return;
    if (begin) {
        plat_mutex_lock(&sc_lock);
        sc_now = VK_NULL_HANDLE;
        return;
    }
    sc_now = sc;
    plat_mutex_unlock(&sc_lock);
}

// vk_device.c after the device (vk_ctx() is not set yet): the waiter when the device has present wait enabled.
void vk_lat_device(VkDevice device, int has_present_wait) {
    if (!vk_lat_on()) return;
    dev = device;
    wait_fn = has_present_wait ? (PFN_vkWaitForPresentKHR)vkGetDeviceProcAddr(device, "vkWaitForPresentKHR") : NULL;
    waits = wait_fn != NULL;
    if (waits && !plat_thread_spawn(waiter_main, NULL, "vk present wait", 0)) waits = 0;
    rt_log("latency: measured to the %s (MKW_LATENCY)", waits ? "screen (present wait)" : "host present, no present wait");
}
