// Skip movies: MKW_SKIP_MOVIES=1 presses A for the player while a cup opening plays, the way
// a player skips it on the console (the input scripts skipped it by accident: MKW_SEQ_HOLD exists for that). Only the
// openings (romfs/Movie/*Op.webm, SV0N_OP.webm): the ad boards in the race and the manuals are movies too.
// A movie starts when its file opens (fs_host.c) and ends when its VP9 decoder is finalized (ov_codec.c), which the
// player does when it stops, skipped or not; the presses stop then, or after MAX_TICKS, so no A reaches what follows.
#include <stdatomic.h>
#include <string.h>

#include "cfg.h"
#include "hacks.h"
#include "host.h"
#include "input_map.h"

#define PRESS_EVERY 12   // ticks between presses: the player reads a press, not a held button
#define PRESS_TICKS 4
#define MAX_TICKS 600    // 10 s of game time: the longest opening is ~7 s (WaitIntroMovie, tck1)

#define OPENING_MAX_NS 15000000000ull   // an opening whose decoder never closes stops counting as one after this

static _Atomic uint64_t since;   // the tick the movie opened at + 1; 0 none
static _Atomic uint64_t opening_at;   // kern_now_ns when an opening's file opened, 0 none (whether skipped or not)

static int is_opening(const char *path) {
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    size_t n = strlen(name);
    return strstr(path, "Movie/") && n > 7 && (!strcmp(name + n - 7, "Op.webm") || !strcmp(name + n - 7, "OP.webm"));
}

// game.c runs a cup opening at interval 1 from its file's open: one opened at 2 or 4 (MKW_FPS 30, 20, 15; under
// MKW_SKIP_MOVIES or the player's own A) was not skipped in 7 of 9 runs, one opened at 1 always was (13 of 13; dn2,
// 15/11/2026). ov_movie_playing() sees it only once its first pictures come out, too late.
int skip_opening_playing(void) {
    uint64_t t = atomic_load_explicit(&opening_at, memory_order_relaxed);
    return t && kern_now_ns() - t < OPENING_MAX_NS;
}

void skip_file_opened(const char *path) {
    if (!is_opening(path)) return;   // the path first: every guest file open comes here
    atomic_store_explicit(&opening_at, kern_now_ns(), memory_order_relaxed);
    if (!cfg_bool("MKW_SKIP_MOVIES")) return;
    atomic_store_explicit(&since, frame_step_ticks() + 1, memory_order_relaxed);
    rt_log("skip: %s opened: pressing A until it ends (MKW_SKIP_MOVIES)", path);
}

void skip_movie_closed(void) {
    atomic_store_explicit(&opening_at, 0, memory_order_relaxed);
    uint64_t t = atomic_exchange_explicit(&since, 0, memory_order_relaxed);
    if (t) rt_log("skip: the movie ended after %llu ticks", (unsigned long long)(frame_step_ticks() + 1 - t));
}

void skip_input(uint64_t *buttons) {
    uint64_t t = atomic_load_explicit(&since, memory_order_relaxed);
    if (!t) return;
    uint64_t age = frame_step_ticks() + 1 - t;
    if (age > MAX_TICKS) {
        atomic_store_explicit(&since, 0, memory_order_relaxed);
        rt_log("skip: no end of the movie after %d ticks: presses stopped", MAX_TICKS);
        return;
    }
    if (age % PRESS_EVERY < PRESS_TICKS) *buttons |= 1ull << B_A;
}
