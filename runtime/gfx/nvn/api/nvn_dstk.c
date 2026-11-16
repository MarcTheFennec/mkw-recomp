// Draw call stacks (diagnosis; ctl "drawstacks [N]"): for the next N presented frames, every draw the game records is
// counted by its guest call stack (the return address and up to five more from the frame-pointer chain on the guest
// stack) and by the depth target bound at the time, which tells the views of a split screen apart. Then one line per
// stack: "dstk: f<frame offset> depth <storage> n<draws> main+<lr> <- main+<lr>...". Two of them, one where an object
// shows and one where it is gone (#430: a parked player's view loses what the other view no longer sees), give the
// game's code that decides what a view draws. Off: one relaxed load per draw.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nvn_api.h"
#include "platform/platform.h"

#define MAIN_BASE 0x7000000000ull
#define DEPTH 6
#define NSLOT 4096
#define STACK_SPAN (8ull << 20)   // a frame pointer this far above sp is taken as off the stack

typedef struct { uint64_t key, depth, lr[DEPTH], n; uint32_t frame, nlr; } Slot;
static Slot slots[NSLOT];
static volatile int armed;
static uint64_t first, last;   // presented frames counted: [first, last)
static PlatMutex lock = PLAT_MUTEX_INIT;
static _Thread_local uint64_t cur_depth;

void nvn_dstk_request(int frames) {
    plat_mutex_lock(&lock);
    memset(slots, 0, sizeof slots);
    first = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + 1;
    last = first + (uint64_t)(frames > 0 ? frames : 1);
    __atomic_store_n(&armed, 1, __ATOMIC_RELEASE);
    plat_mutex_unlock(&lock);
    rt_log("ctl: drawstacks for frames %llu-%llu", (unsigned long long)first, (unsigned long long)last - 1);
}

void nvn_dstk_rt(const void *depth_mem) { cur_depth = (uint64_t)(uintptr_t)depth_mem; }

static void flush(void) {   // lock held
    int n = 0;
    for (int i = 0; i < NSLOT; i++) {
        const Slot *s = &slots[i];
        if (!s->n) continue;
        char b[512];
        int o = snprintf(b, sizeof b, "dstk: f%u depth %#llx n%llu", s->frame, (unsigned long long)s->depth,
                         (unsigned long long)s->n);
        for (uint32_t k = 0; k < s->nlr && o < (int)sizeof b - 32; k++)
            o += snprintf(b + o, sizeof b - (size_t)o, k ? " <- %s%#llx" : " %s%#llx",
                          s->lr[k] >= MAIN_BASE ? "main+" : "", (unsigned long long)(s->lr[k] >= MAIN_BASE ?
                                                                                   s->lr[k] - MAIN_BASE : s->lr[k]));
        rt_log("%s", b);
        n++;
    }
    rt_log("dstk: %d stacks over frames %llu-%llu", n, (unsigned long long)first, (unsigned long long)last - 1);
}

void nvn_dstk_draw(const Cpu *c) {
    if (!__atomic_load_n(&armed, __ATOMIC_ACQUIRE)) return;
    uint64_t f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    if (f < first) return;
    Slot s = {.depth = cur_depth, .lr = {c->x[30]}, .nlr = 1};
    uint64_t fp = c->x[29], sp = c->sp;
    while (s.nlr < DEPTH && fp && !(fp & 7) && fp >= sp && fp - sp < STACK_SPAN) {
        const uint64_t *rec = (const uint64_t *)(uintptr_t)fp;   // {previous fp, return address}
        s.lr[s.nlr++] = rec[1];
        if (rec[0] <= fp) break;
        fp = rec[0];
    }
    plat_mutex_lock(&lock);
    if (!armed) goto out;
    if (f >= last) {
        armed = 0;
        flush();
        goto out;
    }
    s.frame = (uint32_t)(f - first);
    uint64_t h = s.depth * 0x9E3779B97F4A7C15ull ^ s.frame;
    for (uint32_t k = 0; k < s.nlr; k++) h = (h ^ s.lr[k]) * 0x100000001B3ull;
    s.key = h | 1;
    for (uint32_t i = (uint32_t)(h >> 52) % NSLOT, tries = 0; tries < NSLOT; i = (i + 1) % NSLOT, tries++) {
        if (slots[i].key == s.key) { slots[i].n++; break; }
        if (!slots[i].key) { slots[i] = s, slots[i].n = 1; break; }
    }
out:
    plat_mutex_unlock(&lock);
}
