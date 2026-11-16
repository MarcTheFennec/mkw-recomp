// Part of the frame dump (vk_dump.c): MKW_DUMP_DRAWS, a copy of the first colour target after each draw of a selected
// run of passes. MKW_DUMP_DRAWS=<from>-<to> or =<16 hex chars of an fs hash> (diagnosis, with MKW_DUMP or an MKW_CTL
// dump): from the first draw of a pass in that range, or the first draw with that fragment shader, every draw until the
// targets change also copies the first colour target (draw_<n>_t0_....png, one line per draw in passes.txt). A range or
// a shader, because pass numbers move between frames (an fs that draws once a frame just before the race's particles,
// drift7). Copies split and resume rendering with LOAD, so they observe the just-recorded draw; the passes those splits
// end copy no targets (the snapshots stand for them). MKW_DUMP_DRAWS_RECT=x,y,w,h copies only that rectangle of the
// target, so the DRAW_CAP snapshots of a long run (the race's particles, ~120 draws) fit in INFLIGHT_CAP.
// MKW_DUMP_DRAWS_SKIP=N: the run's first N draws are counted but not copied, to cover a pass of more than DRAW_CAP draws
// in parts (#430: a split-screen view's main pass, ~800 draws). Each snapshot names its draw's index in the run and its
// vs and fs, to tell which draw paints a given object.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "vk.h"
#include "vk_exec.h"
#include "platform/platform.h"
#include "vk_dump_internal.h"

#define DRAW_CAP 256                   // draw snapshots per dump
static int draw_lo = -1, draw_hi = -1; // MKW_DUMP_DRAWS=<from>-<to>: passes copied after every draw (-1: none)
static uint32_t draw_rect[4];          // MKW_DUMP_DRAWS_RECT: x, y, w, h of the snapshots (w 0: the whole target)
static int draw_n;                     // snapshots in the selected run
static int draw_skip, draw_seen;       // MKW_DUMP_DRAWS_SKIP; draws of the run so far (copied or not)
static _Thread_local uint64_t draw_vs_h, draw_fs_h;   // the selected draw's shaders, for its snapshot line
static uint64_t draw_fs;               // or the pass that draws with this fragment shader (0: none)
static int draw_run_on;                // latched through replay's splits of the selected pass
static uint64_t draw_targets;          // targets of that pass, to stop at the next pass
static _Thread_local int draw_pending; // this thread recorded a draw selected for a snapshot

void draws_init(void) {
    const char *dp = cfg_get("MKW_DUMP_DRAWS");
    if (dp && strlen(dp) == 16) draw_fs = strtoull(dp, NULL, 16);   // one fragment shader, not a pass range
    else if (dp && sscanf(dp, "%d-%d", &draw_lo, &draw_hi) == 2 && draw_hi - draw_lo > 64) draw_lo = draw_hi = -1;
    const char *dr = cfg_get("MKW_DUMP_DRAWS_RECT");
    if (dr && sscanf(dr, "%u,%u,%u,%u", &draw_rect[0], &draw_rect[1], &draw_rect[2], &draw_rect[3]) != 4) draw_rect[2] = 0;
    if (!draw_rect[3]) draw_rect[2] = 0;
    const char *sk = cfg_get("MKW_DUMP_DRAWS_SKIP");
    draw_skip = sk ? atoi(sk) : 0;
}

void draws_reset(void) { draw_n = draw_run_on = draw_seen = 0; }

void draws_select(VkExec *ex, const VkPipeState *s, int pass) {
    draw_pending = 0;
    if (frame_now() == dump_frame && !draw_run_on) {
        int match = pass >= draw_lo && pass <= draw_hi;
        if (draw_fs && shader_hash(s->fs) == draw_fs) match = 1;
        if (match) draw_run_on = 1, draw_targets = targets_sig(ex);   // draw_n goes on: a later run must not reuse names
    }
    if (draw_run_on && frame_now() == dump_frame) draw_pending = 1, draw_vs_h = shader_hash(s->vs), draw_fs_h = shader_hash(s->fs);
}

const char *draws_pass_end(uint64_t sig, const char *skip) {
    if (draw_run_on && sig != draw_targets) draw_run_on = 0;
    if (draw_run_on && !skip) return "draw snapshots instead";   // a split of the run: its copies stand for it
    return skip;
}

// The caller closes rendering after the actual draw before copying; image-to-buffer copies inside dynamic rendering
// are invalid and otherwise produce a plausible-looking but unchanged PNG.
void vk_dump_draw_after(VkExec *ex) {
    plat_mutex_lock(&rec_lock);
    int seen = draw_pending && draw_run_on && frame_now() == dump_frame ? draw_seen++ : -1;
    if (seen < draw_skip || draw_n >= DRAW_CAP) {
        draw_pending = 0;
        plat_mutex_unlock(&rec_lock);
        return;
    }
    draw_pending = 0;
    draw_n++;
    int n = seen;
    plat_mutex_unlock(&rec_lock);
    int reopen;
    VkCommandBuffer cb = vk_exec_inline_begin(&reopen);
    if (!cb) return;
    const VkExecTarget *t = &ex->color[0];
    if (!t->image || t->samples > 1 || t->is3d) {
        vk_exec_inline_end(reopen);
        return;
    }
    uint32_t x = 0, y = 0, w = t->width, h = t->height;
    if (draw_rect[2] && draw_rect[0] < t->width && draw_rect[1] < t->height) {   // clamped to the target
        x = draw_rect[0], y = draw_rect[1];
        w = draw_rect[2] < t->width - x ? draw_rect[2] : t->width - x;
        h = draw_rect[3] < t->height - y ? draw_rect[3] : t->height - y;
    }
    char fn[16], name[160];
    snprintf(name, sizeof name, "draw_%03d_t0_%ux%u_%d_%s", n, w, h, (int)t->format, fmt_name(t->format, fn));
    const char *why = copy_region(ex, t->image, t->format, t->aspect, t->level, t->base_layer,
                                  (VkOffset3D){(int32_t)x, (int32_t)y, 0}, (VkExtent3D){w, h, 1}, w, h, 1, name);
    plat_mutex_lock(&rec_lock);
    fprintf(passes_f, "    draw snapshot %s.png at %u,%u of %ux%u vs %016llx fs %016llx%s%s\n", name, x, y, t->width,
            t->height, (unsigned long long)draw_vs_h, (unsigned long long)draw_fs_h, why ? " skipped: " : "", why ? why : "");
    plat_mutex_unlock(&rec_lock);
    vk_exec_inline_end(reopen);
}
