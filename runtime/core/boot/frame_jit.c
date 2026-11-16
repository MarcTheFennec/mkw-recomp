// MKW_LOW_LATENCY (Screen tab, live; on by default): the game's render thread gets its next window image (acquire)
// only as early as its frame needs, so the frame it starts, and the controller read its logic takes with it, are
// younger when the picture reaches the screen.
// Why (lat1, lat2, 15/11/2026, Radeon 660M at 15 fps): the game reads the controller as its logic starts a frame and
// presents that frame one logic frame later; its window has three images, and an acquire returned when the image's
// earlier display time plus a period had passed (nvn_api_obj.c), so the render thread ran two frames ahead: 200 ms from
// a read to its display slot, of which the frame's own work needed ~36. Delaying the logic frame instead (lat2) moved
// little: the acquire stayed the gate and only waited less.
// The budget is measured, per frame: from the acquire's return to the GPU finishing the frame's last picture (its slot
// minus the slack the present thread saw, vk_backend.c). An acquire waits until the next slot minus the 90th
// percentile of the last 32 budgets and MARGIN_NS; never before what the window already waits for. A slow frame raises
// the percentile; nothing integrates, so nothing swings. With MKW_LOW_LATENCY=ab it is on in MKW_AB's B blocks only.
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "platform/platform.h"

#define WORKS 32
#define PENDING 8
#define MARGIN_NS 4000000ll   // hypothesis (15/11/2026): checked by the late count of each A/B
#define LOG_NS 5000000000ull

typedef struct { uint64_t slot, acquired; } Pending;
static PlatMutex lk = PLAT_MUTEX_INIT;
static Pending pend[PENDING];   // presented frames waiting for their slack
static unsigned pend_n;
static int64_t work[WORKS];
static unsigned work_n, lates, frames;

static int jit_on(void) {
    const char *v = cfg_value("MKW_LOW_LATENCY");   // "ab" from the environment only: the menu writes 0 or 1
    return !strcmp(v, "ab") ? rt_ab() : cfg_bool("MKW_LOW_LATENCY");
}

void frame_jit_present(uint64_t slot, uint64_t acquired) {
    if (!acquired) return;
    plat_mutex_lock(&lk);
    pend[pend_n++ % PENDING] = (Pending){slot, acquired};
    plat_mutex_unlock(&lk);
}

void frame_jit_slack(uint64_t slot, int64_t slack) {
    plat_mutex_lock(&lk);
    for (unsigned i = 0; i < PENDING; i++)
        if (pend[i].slot == slot && pend[i].acquired) {
            int64_t w = (int64_t)(slot - pend[i].acquired) - slack;   // acquire to GPU done
            if (w > 0) work[work_n++ % WORKS] = w;
            pend[i].acquired = 0;
            break;
        }
    lates += slack < 0, frames++;
    plat_mutex_unlock(&lk);
}

static int cmp_i64(const void *a, const void *b) {
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return (x > y) - (x < y);
}

static int64_t budget(void) {   // lk held: the 90th percentile of the last budgets plus the margin, 0 too few
    if (work_n < WORKS) return 0;
    int64_t v[WORKS];
    memcpy(v, work, sizeof v);
    qsort(v, WORKS, sizeof v[0], cmp_i64);
    return v[WORKS * 9 / 10] + MARGIN_NS;
}

uint64_t frame_jit_start(uint64_t next_slot) {
    static uint64_t log_t0;
    if (!jit_on() || !next_slot) return 0;
    plat_mutex_lock(&lk);
    int64_t b = budget();
    uint64_t now = kern_now_ns();
    if (!log_t0) log_t0 = now;
    if (now - log_t0 >= LOG_NS) {
        rt_log("jit: acquire waits until the slot minus %.1f ms (frame work p90 + margin), %u of %u frames not ready "
               "at their slot (MKW_LOW_LATENCY)", b / 1e6, lates, frames);
        log_t0 = now, lates = frames = 0;
    }
    plat_mutex_unlock(&lk);
    return b && next_slot > (uint64_t)b ? next_slot - (uint64_t)b : 0;
}
