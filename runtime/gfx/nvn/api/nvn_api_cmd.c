// NVN front end, command-buffer slice: command buffers, their memory, recording
// as op records (nvn_obj.h), recording copies behind command handles, and the submit-time resolution of deferred binds
// (nvn_submit_prepare, called by nvnQueueSubmitCommands in nvn_api_core.c). Ported from MacNativeGames
// games/totk/third_party/d2r_gfx/gfx/nvn/hle_nvn.c @ b75be8cf (MIT, Copyright (c) 2026 jhaizhou; origin d2r-mobile),
// lines 647-1062, 1261-1286, 1486-1760 and 1975-1990, and the *Deferred and memory-callback entry points of
// mkw_nvn_ext.c in the same directory. Metal, mach, the translation thread, Apple diagnostics, the per-frame
// statistics and the long-session sweeps (copy marks in control memory, texture pin sweep) dropped.
//
// Records are written into a growable host buffer per command buffer, never into the game's command memory: games hand
// the same command memory to several command buffers recording at once (budgeted for real NVN encodings), and our
// records are larger. The game's command and control memory is accounted only. EndRecording copies the finished
// recording into host memory; the handle is that copy (a flat OP_BEGIN ... OP_END sequence) and lives until the game
// adds the same control memory again. CallCommands and CopyCommands inline the called recording, so copies hold no
// The render-state entry points (payloads of nvn_obj.h) are in nvn_api_state.c; nvn_cmd_internal.h is what they
// share.
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#include "cfg.h"
#include "host.h"
#include "nvn_api.h"
#include "nvn_hmap.h"
#include "nvn_cmd_internal.h"
#include "platform/platform.h"

#define CMD_MAGIC MAGIC('C', 'M', 'D', 'B')
#define COPY_MAGIC 0x59504f43444d43ull     // "CMDCOPY": prefix of a live recording copy
#define OP_RESERVE 24                       // room always kept at the end of the recording buffer
#define MIN_COMMAND_SIZE 512                // NVN_DEVICE_INFO_COMMAND_BUFFER_MIN_COMMAND_SIZE (nvn_api_core.c devinfo)
#define MIN_CONTROL_SIZE 256                // NVN_DEVICE_INFO_COMMAND_BUFFER_MIN_CONTROL_SIZE
#define CTL_PER_RECORDING 64                // control memory one recording is reported to use
#define COPY_PAGE_SHIFT 16                  // recording copy index granularity (release_copies)

// ---------- memory callbacks
// NVN calls the game's memory callback when a recording needs command or control memory and none is left. Records
// live in host memory here, so the callback is called only where the hardware certainly would: a recording starts
// without any command memory, or ends without any control memory. It runs on the calling guest thread through
// rt_call; whatever memory it adds is accounted. Set{Command,Control}MemoryCallbackEnabled(false) suppress it.
enum { CB_NO_COMMAND = 1, CB_NO_CONTROL = 2 };
static PlatMutex cb_lock = PLAT_MUTEX_INIT;
static nvn_hmap cb_off = {.klen = 8};       // command buffer -> disabled callback kinds | 0x100

static unsigned cb_disabled(const cmd_t *o) {
    plat_mutex_lock_shared(&cb_lock);
    uintptr_t v = (uintptr_t)nvn_hm_get(&cb_off, &o);
    plat_mutex_unlock_shared(&cb_lock);
    return (unsigned)v & 0xff;
}

static void cb_set_disabled(const cmd_t *o, unsigned kind, int disabled) {
    plat_mutex_lock(&cb_lock);
    unsigned v = (unsigned)(uintptr_t)nvn_hm_get(&cb_off, &o) & 0xff;
    v = disabled ? v | kind : v & ~kind;
    if (v) nvn_hm_put(&cb_off, &o, (void *)(uintptr_t)(v | 0x100));
    else nvn_hm_del(&cb_off, &o);
    plat_mutex_unlock(&cb_lock);
}

static void memory_event(Cpu *c, cmd_t *o, int event, uint64_t min) {
    if (!o->cb || (cb_disabled(o) & (event == NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_COMMAND_MEMORY ? CB_NO_COMMAND
                                                                                                     : CB_NO_CONTROL)))
        return;
    NVN_LOG("command buffer %p: memory callback %p (%s memory, %llu bytes)", (void *)o, (void *)o->cb,
            event == NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_COMMAND_MEMORY ? "command" : "control",
            (unsigned long long)min);
    rt_call(c, (uint64_t)(uintptr_t)o->cb, 4, (uint64_t)(uintptr_t)o, (uint64_t)event, min,
            (uint64_t)(uintptr_t)o->cb_data);
}

// ---------- recording buffer
static void put_hdr(uint8_t *p, uint32_t op, uint32_t size) {
    op_hdr h = {op, size};
    memcpy(p, &h, sizeof h);
}

static void cmd_reserve(cmd_t *o, size_t need) {
    if (o->seg && o->cur + need + OP_RESERVE <= o->end) return;
    size_t used = o->seg ? (size_t)(o->cur - o->seg) : 0;
    size_t cap = o->seg ? (size_t)(o->end - o->seg) : 0;
    if (!cap) cap = 64 << 10;
    while (used + need + OP_RESERVE > cap) cap *= 2;
    uint8_t *b = realloc(o->seg, cap);
    if (!b) rt_fatal("nvn: command recording buffer of %zu bytes", cap);
    o->seg = b;
    o->cur = b + used;
    o->end = b + cap;
}

// Space for one record (header included); the header's op is filled by the caller.
static uint8_t *cmd_alloc(cmd_t *o, uint32_t size) {
    size = (size + 7) & ~7u;
    cmd_reserve(o, size);
    uint8_t *p = o->cur;
    o->cur += size;
    o->cmd_used += size;
    o->hw_used += size;
    o->last_rec = size;
    put_hdr(p, 0, size);
    return p;
}

// The game sums nvnCommandBufferGetCommandMemoryUsed over recordings to size resource heaps, so usage must be what the
// real NVN push-buffer encoding takes, not our record size. Known encodings (Maxwell methods, from a real recording):
// DrawElements 12 words, DrawElementsBaseVertex 11, BindVertexAttribState 1+16, BindVertexStreamState 16+n,
// BindVertexBuffer 6 words each.
void cmd_hw_fix(cmd_t *o, size_t bytes) {
    o->hw_used += bytes;
    o->hw_used -= o->last_rec;
    o->last_rec = bytes;
}

// A zeroed payload of one record.
void *cmd_rec(cmd_t *o, uint32_t op, size_t payload) {
    uint32_t sz = (uint32_t)((8 + payload + 7) & ~(size_t)7);
    uint8_t *p = cmd_alloc(o, sz);
    put_hdr(p, op, sz);
    memset(p + 8, 0, sz - 8);
    return p + 8;
}

// Only what the view sets: the game builds views on its stack, and the fields of a part it does not set (and the
// padding) hold whatever was there, different every frame. Every reader checks the set_ flags; caches that compare
// views by their bytes (MKW_REUSE's key: 1.5 % of render-target states repeated two frames later, ru10) need the rest
// zero.
void cmd_copy_view(view_t *dst, uint8_t *has, const NVNtextureView *v) {
    *has = v != NULL;
    if (!v) return;
    const view_t *s = (const view_t *)v;
    memset(dst, 0, sizeof *dst);
    dst->set_levels = s->set_levels, dst->set_layers = s->set_layers, dst->set_format = s->set_format;
    dst->set_target = s->set_target, dst->set_swizzle = s->set_swizzle, dst->set_dsmode = s->set_dsmode;
    if (s->set_levels) dst->base_level = s->base_level, dst->nlevels = s->nlevels;
    if (s->set_layers) dst->min_layer = s->min_layer, dst->nlayers = s->nlayers;
    if (s->set_format) dst->format = s->format;
    if (s->set_target) dst->target = s->target;
    if (s->set_swizzle) memcpy(dst->swz, s->swz, sizeof dst->swz);
    if (s->set_dsmode) dst->dsmode = s->dsmode;
}

// ---------- texture pins
// Recordings refer to textures by pointer, but the game may finalize or reuse an NVNtexture after recording and before
// the submit (NVN itself captured only GPU state). Records therefore point at an immutable host copy per texture
// initialization (tex_t.serial). The copies are kept for the whole run (MacNativeGames sweeps them for long sessions).
static PlatMutex pin_lock = PLAT_MUTEX_INIT;
static nvn_hmap pins = {.klen = 8};         // serial -> tex_t copy

const tex_t *cmd_tex_pin(const NVNtexture *nt) {
    const tex_t *t = (const tex_t *)nt;
    if (!t) return NULL;
    uint64_t serial = t->serial;
    if (!serial) rt_fatal("nvn: a recording references the uninitialized texture object %p", (const void *)t);
    plat_mutex_lock_shared(&pin_lock);
    const tex_t *r = nvn_hm_get(&pins, &serial);
    plat_mutex_unlock_shared(&pin_lock);
    if (r) return r;
    plat_mutex_lock(&pin_lock);
    r = nvn_hm_get(&pins, &serial);
    if (!r) {
        tex_t *copy = malloc(sizeof *copy);
        if (!copy) rt_fatal("nvn: texture pin copy");
        *copy = *t;
        nvn_hm_put(&pins, &serial, copy);
        r = copy;
    }
    plat_mutex_unlock(&pin_lock);
    return r;
}

// ---------- recording copies
// A handle points just past a copy_prefix. Copies are indexed by the 64 KiB block of their control-memory position, so
// adding control memory again releases the copies recorded there without walking every live copy. 4 KiB pages cost
// about 500 lookups per AddControlMemory under the exclusive lock (1.4.3 field: 53 calls a frame, 2 MiB ranges, 3.7
// copies per page), and the recording threads waited on it.
typedef struct { uint64_t magic, bytes, made_at, used_at, deferred; } copy_prefix;   // deferred: 0 unknown, 1 none, 2 some
_Static_assert(sizeof(copy_prefix) % 8 == 0, "records stay 8-byte aligned");
typedef struct { uintptr_t lo, hi; uint8_t *copy; } cmd_copy;
typedef struct { cmd_copy *v; uint32_t n, cap; } copy_bucket;
static PlatMutex copy_lock = PLAT_MUTEX_INIT;    // guards the two maps, the counters and every prefix
static nvn_hmap copy_pages = {.klen = 8};   // page -> copy_bucket
static nvn_hmap copy_live = {.klen = 8};    // handle -> copy_prefix of a live copy
static uint64_t ncopies, copy_bytes, copies_made, nsubmits;
// Asynchronous replay: copies released by the game, and per-submission clones, that a queued submission still reads.
// Freed once gfx_replayed_submits() reaches their used_at. Empty with synchronous replay.
static copy_prefix **zombies;
static uint32_t nzombies, cap_zombies;

static void zombie_add(copy_prefix *pf) {   // copy_lock held
    if (nzombies == cap_zombies) {
        cap_zombies = cap_zombies ? cap_zombies * 2 : 64;
        zombies = realloc(zombies, sizeof *zombies * cap_zombies);
        if (!zombies) rt_fatal("nvn: out of memory (released recording copies)");
    }
    zombies[nzombies++] = pf;
}

static void zombie_sweep(uint64_t replayed) {   // copy_lock held
    for (uint32_t i = 0; i < nzombies;) {
        if (zombies[i]->used_at <= replayed) {
            free(zombies[i]);
            zombies[i] = zombies[--nzombies];
        } else {
            i++;
        }
    }
}

static copy_prefix *copy_find(uint64_t handle) {   // copy_lock held
    return nvn_hm_get(&copy_live, &handle);
}

int nvn_handle_live(uint64_t handle) {
    plat_mutex_lock_shared(&copy_lock);
    int live = copy_find(handle) != NULL;
    plat_mutex_unlock_shared(&copy_lock);
    return live;
}

uint64_t nvn_handle_bytes(uint64_t handle) {
    plat_mutex_lock_shared(&copy_lock);
    const copy_prefix *pf = copy_find(handle);
    uint64_t n = pf ? pf->bytes : 0;
    plat_mutex_unlock_shared(&copy_lock);
    return n;
}

// Marks a live copy used by submission now; its size from OP_BEGIN to OP_END inclusive.
static uint64_t copy_touch(uint64_t handle, uint64_t now, const char *what) {
    plat_mutex_lock_shared(&copy_lock);
    copy_prefix *pf = copy_find(handle);
    uint64_t bytes = 0;
    if (pf) __atomic_store_n(&pf->used_at, now, __ATOMIC_RELAXED), bytes = pf->bytes;
    plat_mutex_unlock_shared(&copy_lock);
    if (!pf) rt_fatal("nvn: %s: command handle %#llx is not a live recording (released or never recorded)", what,
                      (unsigned long long)handle);
    return bytes;
}

// The recording [p, p + n), OP_BEGIN to OP_END (the recording buffer is one block: recording emits no OP_JUMP), copied
// into one host block of its exact size. One allocation and one copy: growing the copy record by record cost the
// game's main thread about 1 ms a frame in reallocations and small copies (1.4.3, field).
static uint8_t *flatten_recording(const uint8_t *p, size_t n) {
    uint8_t *out = malloc(sizeof(copy_prefix) + n);
    if (!out) rt_fatal("nvn: command copy allocation");
    memcpy(out + sizeof(copy_prefix), p, n);
    for (size_t at = 0;;) {   // the records stay checked: a corrupt one stops here, not in the replay
        op_hdr h;
        memcpy(&h, p + at, sizeof h);
        if (h.size < 8 || (h.size & 7) || h.op >= OP_COUNT_ || h.op == OP_JUMP || h.op == OP_CALL || at + h.size > n ||
            (!at && h.op != OP_BEGIN))
            rt_fatal("nvn: corrupt recording at %p (op %u size %u)", (const void *)(p + at), h.op, h.size);
        at += h.size;
        if (h.op == OP_END) {
            if (at != n) rt_fatal("nvn: recording at %p ends at %zu of %zu bytes", (const void *)p, at, n);
            break;
        }
    }
    *(copy_prefix *)out = (copy_prefix){COPY_MAGIC, n, 0, 0};
    return out + sizeof(copy_prefix);
}

static void copy_add(uintptr_t lo, uintptr_t hi, uint8_t *copy) {
    copy_prefix *pf = (copy_prefix *)copy - 1;
    uint64_t handle = (uintptr_t)copy;
    uintptr_t page = lo >> COPY_PAGE_SHIFT;
    plat_mutex_lock(&copy_lock);
    pf->made_at = nsubmits;
    copy_bucket *b = nvn_hm_get(&copy_pages, &page);
    if (!b) {
        b = calloc(1, sizeof *b);
        if (!b) rt_fatal("nvn: command copy index");
        nvn_hm_put(&copy_pages, &page, b);
    }
    if (b->n == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 8;
        b->v = realloc(b->v, b->cap * sizeof *b->v);
        if (!b->v) rt_fatal("nvn: command copy index");
    }
    b->v[b->n++] = (cmd_copy){lo, hi, copy};
    nvn_hm_put(&copy_live, &handle, pf);
    ncopies++;
    copy_bytes += pf->bytes;
    if (++copies_made % 20000 == 0)
        NVN_LOG("recording copies: %llu made, %llu live, %.1f MiB, submit %llu", (unsigned long long)copies_made,
                (unsigned long long)ncopies, copy_bytes / 1048576.0, (unsigned long long)nsubmits);
    plat_mutex_unlock(&copy_lock);
}

// Adding control memory again retires every recording copy made in it (the driver's handle lived there).
static void release_copies(uintptr_t lo, uintptr_t hi) {
    if (hi <= lo) return;
    uint64_t replayed = gfx_replayed_submits();
    plat_mutex_lock(&copy_lock);
    if (nzombies) zombie_sweep(replayed);
    for (uintptr_t pg = lo >> COPY_PAGE_SHIFT; pg <= (hi - 1) >> COPY_PAGE_SHIFT; pg++) {
        copy_bucket *b = nvn_hm_get(&copy_pages, &pg);
        if (!b) continue;
        for (uint32_t i = 0; i < b->n;) {
            cmd_copy *k = &b->v[i];
            if (!(k->lo < hi && lo < k->hi)) { i++; continue; }
            uint64_t handle = (uintptr_t)k->copy;
            copy_prefix *pf = (copy_prefix *)k->copy - 1;
            nvn_hm_del(&copy_live, &handle);
            ncopies--;
            copy_bytes -= pf->bytes;
            pf->magic = 0;
            if (pf->used_at > replayed) zombie_add(pf);   // a queued submission still reads it
            else free(pf);
            b->v[i] = b->v[--b->n];
        }
        if (!b->n) {
            nvn_hm_del(&copy_pages, &pg);
            free(b->v);
            free(b);
        }
    }
    plat_mutex_unlock(&copy_lock);
}

// ---------- CommandBuffer: object and memory
// NVNboolean nvnCommandBufferInitialize(NVNcommandBuffer *cmd, NVNdevice *device): the inline fast-path fields are kept
// at ~0 / 0 so NVN 55 inline code never finds room and calls the API (nvn_obj.h cmd_t)
NVN_API(nvnCommandBufferInitialize) {
    cmd_t *o = cmd_of(c);
    memset(o, 0, sizeof *o);
    o->magic = CMD_MAGIC;
    o->dev = arg_p(c, 1);
    o->inline_cur = ~0ull;
    o->inline_end = 0;
    cb_set_disabled(o, CB_NO_COMMAND | CB_NO_CONTROL, 0);
    ret_x(c, 1);
}
// void nvnCommandBufferFinalize(NVNcommandBuffer *cmd): handles outlive their recorder
NVN_API(nvnCommandBufferFinalize) {
    cmd_t *o = cmd_of(c);
    free(o->seg);
    o->seg = o->cur = o->end = NULL;
    o->magic = 0;
    cb_set_disabled(o, CB_NO_COMMAND | CB_NO_CONTROL, 0);
}
// void nvnCommandBufferSetMemoryCallback(NVNcommandBuffer *cmd, PFNNVNCOMMANDBUFFERMEMORYCALLBACKPROC callback)
NVN_API(nvnCommandBufferSetMemoryCallback) { cmd_of(c)->cb = arg_p(c, 1); }
// void nvnCommandBufferSetMemoryCallbackData(NVNcommandBuffer *cmd, void *data)
NVN_API(nvnCommandBufferSetMemoryCallbackData) { cmd_of(c)->cb_data = arg_p(c, 1); }
// PFNNVNCOMMANDBUFFERMEMORYCALLBACKPROC nvnCommandBufferGetMemoryCallback(const NVNcommandBuffer *cmd)
NVN_API(nvnCommandBufferGetMemoryCallback) { ret_x(c, (uintptr_t)cmd_of(c)->cb); }
// void *nvnCommandBufferGetMemoryCallbackData(const NVNcommandBuffer *cmd) (nvn.h declares it void)
NVN_API(nvnCommandBufferGetMemoryCallbackData) { ret_x(c, (uintptr_t)cmd_of(c)->cb_data); }
// void nvnCommandBufferSetCommandMemoryCallbackEnabled / SetControlMemoryCallbackEnabled(NVNcommandBuffer *cmd,
// NVNboolean enabled) (NVN 55, no prototype in nvn.h)
NVN_API(nvnCommandBufferSetCommandMemoryCallbackEnabled) { cb_set_disabled(cmd_of(c), CB_NO_COMMAND, !nvn_arg_bool(c, 1)); }
NVN_API(nvnCommandBufferSetControlMemoryCallbackEnabled) { cb_set_disabled(cmd_of(c), CB_NO_CONTROL, !nvn_arg_bool(c, 1)); }
// void nvnCommandBufferAddCommandMemory(NVNcommandBuffer *cmd, const NVNmemoryPool *pool, ptrdiff_t offset, size_t size):
// accounting only; command memory can be shared by independent recordings with distinct control memory
NVN_API(nvnCommandBufferAddCommandMemory) {
    cmd_t *o = cmd_of(c);
    const pool_t *p = arg_p(c, 1);
    o->cmd_base = (uintptr_t)(p ? p->mem : NULL) + (uintptr_t)arg_x(c, 2);
    o->game_size = arg_x(c, 3);
    o->cmd_used = 0;
    o->hw_used = 0;
}
// void nvnCommandBufferAddControlMemory(NVNcommandBuffer *cmd, void *memory, size_t size): the SDK encodes handles in
// control memory (sdk+0x444530), so reusing that storage retires the copies recorded there
NVN_API(nvnCommandBufferAddControlMemory) {
    cmd_t *o = cmd_of(c);
    void *m = arg_p(c, 1);
    size_t n = arg_x(c, 2);
    release_copies((uintptr_t)m, (uintptr_t)m + n);
    o->ctl = m;
    o->ctl_size = n;
    o->ctl_used = 0;
}
// size_t nvnCommandBufferGet{Control,Command}Memory{Size,Used,Free}(const NVNcommandBuffer *cmd): usage is bounded by
// the memory the game gave, so the game never sees more used than it provided
static size_t min_size(size_t a, size_t b) { return a < b ? a : b; }
NVN_API(nvnCommandBufferGetControlMemorySize) { ret_x(c, cmd_of(c)->ctl_size); }
NVN_API(nvnCommandBufferGetControlMemoryUsed) { const cmd_t *o = cmd_of(c); ret_x(c, min_size(o->ctl_used, o->ctl_size)); }
NVN_API(nvnCommandBufferGetControlMemoryFree) { const cmd_t *o = cmd_of(c); ret_x(c, o->ctl_size - min_size(o->ctl_used, o->ctl_size)); }
NVN_API(nvnCommandBufferGetCommandMemorySize) { ret_x(c, cmd_of(c)->game_size); }
NVN_API(nvnCommandBufferGetCommandMemoryUsed) { const cmd_t *o = cmd_of(c); ret_x(c, min_size(o->hw_used, o->game_size)); }
NVN_API(nvnCommandBufferGetCommandMemoryFree) { const cmd_t *o = cmd_of(c); ret_x(c, o->game_size - min_size(o->hw_used, o->game_size)); }

// ---------- CommandBuffer: recording
// void nvnCommandBufferBeginRecording(NVNcommandBuffer *cmd)
NVN_API(nvnCommandBufferBeginRecording) {
    cmd_t *o = cmd_of(c);
    if (o->recording) rt_log("nvn: BeginRecording on command buffer %p while it records", (void *)o);
    o->recording = 1;
    o->cur = o->seg;   // the previous recording was copied at EndRecording
    o->rec_start = o->cmd_used;
    put_hdr(cmd_alloc(o, 8), OP_BEGIN, 8);
    o->hw_used -= 8;   // BEGIN and END have no hardware equivalent
    o->hw_start = o->hw_used;
    if (!o->game_size) memory_event(c, o, NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_COMMAND_MEMORY, MIN_COMMAND_SIZE);
}
// NVNcommandHandle nvnCommandBufferEndRecording(NVNcommandBuffer *cmd)
NVN_API(nvnCommandBufferEndRecording) {
    cmd_t *o = cmd_of(c);
    if (!o->recording || !o->seg) rt_fatal("nvn: EndRecording on command buffer %p, which does not record", (void *)o);
    if (!o->ctl_size) memory_event(c, o, NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_CONTROL_MEMORY, MIN_CONTROL_SIZE);
    put_hdr(cmd_alloc(o, 8), OP_END, 8);
    o->hw_used -= 8;
    o->recording = 0;
    uint8_t *copy = flatten_recording(o->seg, (size_t)(o->cur - o->seg));
    // Finalizing the recorder does not retire its handle; only reusing the control memory it was recorded in does.
    uintptr_t lo = (uintptr_t)o->ctl + (o->ctl_used < o->ctl_size ? o->ctl_used : 0);
    uintptr_t hi = o->ctl ? lo + 1 : 0;
    o->ctl_used += CTL_PER_RECORDING;
    copy_add(lo, hi, copy);
    ret_x(c, (uintptr_t)copy);
}
// NVNboolean nvnCommandBufferIsRecording(const NVNcommandBuffer *cmd)
NVN_API(nvnCommandBufferIsRecording) { ret_x(c, cmd_of(c)->recording != 0); }

// The called recording is copied into the caller at the call position: the game recycles command memory in small steps,
// so a handle may be gone by the time the caller is submitted. Deferred binds stay deferred. A copy is OP_BEGIN, its
// records and OP_END (flatten_recording checked them), so the records go over as one block; usage grows by the same
// bytes as record by record.
static void inline_recording(cmd_t *o, uint64_t handle, const char *what) {
    if (!handle) rt_fatal("nvn: %s with a null handle", what);
    uint64_t bytes = copy_touch(handle, __atomic_load_n(&nsubmits, __ATOMIC_RELAXED), what);
    if (bytes <= 16) return;
    size_t body = (size_t)bytes - 16;
    cmd_reserve(o, body);
    memcpy(o->cur, (const uint8_t *)(uintptr_t)handle + 8, body);
    o->cur += body;
    o->cmd_used += body;
    o->hw_used += body;
    o->last_rec = 0;
}
// void nvnCommandBuffer{Call,Copy}Commands(NVNcommandBuffer *cmd, int n, const NVNcommandHandle *handles)
NVN_API(nvnCommandBufferCallCommands) {
    cmd_t *o = cmd_of(c);
    int n = (int)arg_x(c, 1);
    const uint64_t *h = arg_p(c, 2);
    for (int i = 0; i < n; i++) inline_recording(o, h[i], "CallCommands");
}
NVN_API(nvnCommandBufferCopyCommands) {
    cmd_t *o = cmd_of(c);
    int n = (int)arg_x(c, 1);
    const uint64_t *h = arg_p(c, 2);
    for (int i = 0; i < n; i++) inline_recording(o, h[i], "CopyCommands");
}

// ---------- CommandBuffer: commands with CPU-visible results (performed by the back end in GPU order)
// void nvnCommandBufferSignalEvent(NVNcommandBuffer *cmd, const NVNevent *event, NVNeventSignalMode mode,
// NVNeventSignalLocation location, int flags, uint32_t value)
NVN_API(nvnCommandBufferSignalEvent) {
    const ev_t *e = arg_p(c, 1);
    *REC(cmd_of(c), OP_SIGNAL_EVENT, p_signal_event) = (p_signal_event){e->val, (uint32_t)arg_x(c, 2), (uint32_t)arg_x(c, 5)};
}
// void nvnCommandBufferResetCounter(NVNcommandBuffer *cmd, NVNcounterType type)
NVN_API(nvnCommandBufferResetCounter) { REC(cmd_of(c), OP_RESET_COUNTER, p_reset_counter)->type = (uint32_t)arg_x(c, 1); }
// void nvnCommandBufferReportCounter(NVNcommandBuffer *cmd, NVNcounterType type, NVNbufferAddress address)
NVN_API(nvnCommandBufferReportCounter) {
    *REC(cmd_of(c), OP_REPORT_COUNTER, p_report_counter) = (p_report_counter){(uint32_t)arg_x(c, 1), arg_x(c, 2)};
}
// void nvnCommandBufferCopyBufferToBuffer(NVNcommandBuffer *cmd, NVNbufferAddress src, NVNbufferAddress dst,
// size_t size, int flags)
NVN_API(nvnCommandBufferCopyBufferToBuffer) {
    *REC(cmd_of(c), OP_COPY_B2B, p_copy_b2b) = (p_copy_b2b){arg_x(c, 1), arg_x(c, 2), arg_x(c, 3)};
}
// void nvnCommandBufferUpdateUniformBuffer(NVNcommandBuffer *cmd, NVNbufferAddress buffer, size_t buffer_size,
// ptrdiff_t offset, size_t size, const void *data): the data is copied at record time (the caller may change it
// afterwards) and written into the buffer at replay
NVN_API(nvnCommandBufferUpdateUniformBuffer) {
    uint64_t dst = arg_x(c, 1) + arg_x(c, 3), n = arg_x(c, 4);
    const void *data = arg_p(c, 5);
    if (n > (16u << 20)) rt_fatal("nvn: UpdateUniformBuffer of %#llx bytes", (unsigned long long)n);
    p_update_ub *r = cmd_rec(cmd_of(c), OP_UPDATE_UB, offsetof(p_update_ub, data) + n);
    r->dst = dst;
    r->size = n;
    memcpy(r->data, data, n);
}

// void nvnCommandBufferPush{Graphics,Compute}Constants[Deferred](NVNcommandBuffer *cmd, int stage, ptrdiff_t offset,
// size_t size, const void *data): the driver writes the data into its own constant bank, which GLSLC-compiled shaders
// read as c24 (5 of the 11 565 programs of 1.8.0; they read zeros before this op). Stage and size are clamped as the
// driver clamps them (sdk+0x5d34b0: stage & 7, size to 256 bytes). The data is copied at record time, for the
// Deferred forms too; MKW only calls the plain graphics form (stage 0 offset 0 size 16, title and race runs of
// 15/11/2026), so a Deferred caller changing the data after the call would need a submit-time read like the
// deferred binds above.
// Compute's constants are kept as stage 5 (NVN_STAGE_CS, vk_draw_internal.h) whatever stage the game passes: MKW's
// photo mode pushes them with stage 0 (ph8, 16/11/2026: "PushComputeConstants stage 0 offset 0 size 32"), which as
// the vertex stage reached no compute program. The replay puts them in the dispatch's c0 (vk_exec_bind.c c0_info).
// A graphics push reaches every graphics stage in the replay (vk_exec_ops.c OP_PUSH_CONST): MKW's SMAA passes push with
// stage 0 and read the bank in their fragment shaders too.
#define PUSH_STAGE_CS 5
static void push_const(Cpu *c, int compute) {
    uint64_t off = arg_x(c, 2), n = arg_x(c, 3);
    if (off >= NVN_C24_BYTES) return;
    if (n > NVN_C24_BYTES - off) n = NVN_C24_BYTES - off;
    p_push_const *r = cmd_rec(cmd_of(c), OP_PUSH_CONST, offsetof(p_push_const, data) + n);
    r->stage = compute ? PUSH_STAGE_CS : (int32_t)(arg_x(c, 1) & 7);
    r->offset = (int32_t)off;
    r->size = (int32_t)n;
    r->pad_ = 0;
    memcpy(r->data, arg_p(c, 4), n);
}
NVN_API(nvnCommandBufferPushGraphicsConstants) { push_const(c, 0); }
NVN_API(nvnCommandBufferPushGraphicsConstantsDeferred) { push_const(c, 0); }
NVN_API(nvnCommandBufferPushComputeConstants) { push_const(c, 1); }
NVN_API(nvnCommandBufferPushComputeConstantsDeferred) { push_const(c, 1); }

// ---------- submit
// What the console reads at the submit call is read here, on the game thread: every handle must be a live recording,
// and deferred binds take the handles the game's arrays hold now (the game fills its tables for the next frame as soon
// as the submit returns). The result is written into the recording copy. Indirect draws without a count take a copy of
// their arguments too (p_draw_idx_ind.snap): the console's GPU reads them right after the submit, the replay later.
static uint64_t def_reads, def_changed, n_clones;
// What the submission being prepared holds (game thread, under the front end's submit lock): samples-passed counter
// reports, other reports and event signals, for MKW_SUBMIT_CATCHUP=report and its census (nvn_api_core.c).
uint32_t nvn_sub_sp_reports, nvn_sub_reports, nvn_sub_events, nvn_sub_waits;

// MKW_REPORT_AT_SUBMIT (default 1; "ab"): counter reports other than samples passed (timestamps) are written
// at the submit, on the game thread, and not again at the fence. Ours went in at the fence with the host time, about a
// frame late and with every report of a submission at nearly the same time; a game that times the GPU with them reads
// old or equal ones (hypothesis for the dark trees, 06/11/2026: the CPU picks another instance set for one frame).
// ts1: two thirds fewer dark trees (17 in the first 1,000 race frames, 43-58 without). =2 (a measurement): the reports
// of a burst 1 us apart, a GPU never over budget: 57 (ts2), so the game decides what it draws from these values.
// 0: written at the fence with the host time, as before.
int nvn_report_at_submit(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = cfg_get("MKW_REPORT_AT_SUBMIT");
        on = e && !strcmp(e, "2") ? 2 : rt_knob("REPORT_AT_SUBMIT", "1");
        rt_log("nvn: timestamp reports written %s (MKW_REPORT_AT_SUBMIT=%d)", on ? "at the submit" : "at the fence", on);
    }
    return on;
}
volatile uint32_t *nvn_sub_event_val[64];   // the first 64 events the submission signals

// MKW_EARLY_READ (core/memory/memory_early.c): what this submit's GPU work writes in CPU-visible memory, listed by the walk
// below and watched from the submit (nvnQueueSubmitCommands) with its fence. Under the submit lock.
NvnEarlyDst nvn_sub_dst[NVN_EARLY_MAX];
int nvn_sub_ndst;
static int early_watch;

static void early_note(uint64_t a, uint64_t n, int kind) {
    if (!a || !n || nvn_sub_ndst == NVN_EARLY_MAX || nvn_gpu_only(a)) return;
    nvn_sub_dst[nvn_sub_ndst++] = (NvnEarlyDst){a, n, kind};
}

static void early_op(uint32_t op, const uint8_t *a) {
    switch (op) {
    case OP_REPORT_COUNTER: {
        const p_report_counter *r = (const p_report_counter *)a;
        if (r->type == NVN_COUNTER_TYPE_SAMPLES_PASSED || !nvn_report_at_submit()) early_note(r->addr, 16, 0);
        break;
    }
    case OP_COPY_B2B: early_note(((const p_copy_b2b *)a)->dst, ((const p_copy_b2b *)a)->size, 1); break;
    case OP_COPY_T2B: {   // src holds the destination buffer; 16 bytes a texel at most
        const p_copy_b2t *t = (const p_copy_b2t *)a;
        uint64_t n = (uint64_t)t->r.width * (uint64_t)t->r.height * (uint64_t)(t->r.depth > 0 ? t->r.depth : 1) * 16;
        early_note(t->src, n < (4u << 20) ? n : (4u << 20), 2);
        break;
    }
    case OP_CLEAR_BUF: early_note(((const p_clear_buf *)a)->addr, ((const p_clear_buf *)a)->size, 3); break;
    case OP_BIND_SSBO: {   // the shader may only read it: told apart by what the game touches
        const p_bind_buf *b = (const p_bind_buf *)a;
        early_note(b->addr, b->size < (64u << 10) ? b->size : (64u << 10), 4);
        break;
    }
    case OP_UPDATE_UB: early_note(((const p_update_ub *)a)->dst, ((const p_update_ub *)a)->size, 5); break;
    default: break;
    }
}

static int resolve_deferred(const uint8_t *p, int depth) {   // returns the deferred binds and snapped draws
    int nd = 0;
    for (;;) {
        op_hdr h;
        memcpy(&h, p, sizeof h);
        if (h.size < 8 || (h.size & 7) || h.op >= OP_COUNT_)
            rt_fatal("nvn: corrupt recording at %p while resolving deferred binds (op %u size %u)", (const void *)p,
                     h.op, h.size);
        if (h.op == OP_END) return nd;
        if (early_watch) early_op(h.op, p + 8);
        if (h.op == OP_JUMP) {
            memcpy(&p, p + 8, 8);
            continue;
        }
        if (h.op == OP_CALL) {
            uint64_t t;
            memcpy(&t, p + 8, 8);
            if (depth >= 16) rt_fatal("nvn: deferred bind resolution: call depth over 16");
            nd += resolve_deferred((const uint8_t *)(uintptr_t)t, depth + 1);
        } else if (h.op == OP_REPORT_COUNTER) {
            const p_report_counter *r = (const p_report_counter *)(uintptr_t)(p + 8);
            if (r->type == NVN_COUNTER_TYPE_SAMPLES_PASSED) nvn_sub_sp_reports++;
            else {
                nvn_sub_reports++;
                if (nvn_report_at_submit() && !nvn_gpu_only(r->addr)) {   // {counter 0, timestamp}
                    // =2: 1 us after the previous report, back to the real time after 8 ms without one (a new frame)
                    static uint64_t fast, last;   // game thread, the front end's submit lock
                    uint64_t now = rt_ticks();   // the game's clock (GetSystemTick, turbo included)
                    fast = last && now - last < 8000000 / RT_TICK_NS ? fast + 1000 / RT_TICK_NS : now;
                    last = now;
                    uint64_t rec[2] = {0, nvn_report_at_submit() == 2 ? fast : now};
                    memcpy((void *)(uintptr_t)r->addr, rec, sizeof rec);
                    nvn_guest_written(r->addr, sizeof rec);
                }
            }
        } else if (h.op == OP_SIGNAL_EVENT) {
            if (nvn_sub_events < 64) nvn_sub_event_val[nvn_sub_events] = ((const p_signal_event *)(uintptr_t)(p + 8))->val;
            nvn_sub_events++;
        } else if (h.op == OP_WAIT_EVENT) {
            nvn_sub_waits++;
        } else if (h.op == OP_DRAW_IDX_IND) {   // the arguments, read when the console's GPU would
            p_draw_idx_ind *d = (p_draw_idx_ind *)(uintptr_t)(p + 8);
            if (!d->count && d->ind && !(d->ind & 3) && !nvn_gpu_only(d->ind)) {
                nd++;
                memcpy(d->snap, (const void *)(uintptr_t)d->ind, d->itype >= 0 ? 20 : 16);
                d->snapped = 1;
            }
        } else if (h.op == OP_BIND_TEX || h.op == OP_BIND_IMG || h.op == OP_BIND_SAMP) {
            p_bind_handles *b = (p_bind_handles *)(uintptr_t)(p + 8);
            if (b->src && b->n > 0 && b->n <= 32) {
                nd++;
                uint64_t cur[32];
                memcpy(cur, (const void *)(uintptr_t)b->src, 8 * (size_t)b->n);
                __atomic_fetch_add(&def_reads, 1, __ATOMIC_RELAXED);
                if (memcmp(cur, b->h, 8 * (size_t)b->n)) {
                    if (__atomic_fetch_add(&def_changed, 1, __ATOMIC_RELAXED) < 6)
                        NVN_LOG("deferred bind read new handles at submit: stage %d slot %d, %#llx at record -> %#llx",
                                b->stage, b->first, (unsigned long long)b->h[0], (unsigned long long)cur[0]);
                    memcpy(b->h, cur, 8 * (size_t)b->n);
                }
            }
        }
        p += h.size;
    }
}

void nvn_submit_prepare(const uint64_t *handles, int n) {
    nvn_sub_sp_reports = nvn_sub_reports = nvn_sub_events = nvn_sub_waits = 0;
    nvn_sub_ndst = 0, early_watch = mem_early_on();
    uint64_t now = __atomic_add_fetch(&nsubmits, 1, __ATOMIC_RELAXED);
    for (int i = 0; i < n; i++) copy_touch(handles[i], now, "QueueSubmitCommands");
    for (int i = 0; i < n; i++) resolve_deferred((const uint8_t *)(uintptr_t)handles[i], 0);
}

// Asynchronous replay. A copy a queued submission still reads keeps that submission's deferred binds: when it holds
// deferred binds (or is not known to hold none), this submission gets a clone, resolved here and freed after its replay.
void nvn_submit_prepare_async(const uint64_t *handles, uint64_t *out, int n) {
    nvn_sub_sp_reports = nvn_sub_reports = nvn_sub_events = nvn_sub_waits = 0;
    nvn_sub_ndst = 0, early_watch = mem_early_on();
    uint64_t now = __atomic_add_fetch(&nsubmits, 1, __ATOMIC_RELAXED);
    uint64_t replayed = gfx_replayed_submits();
    plat_mutex_lock(&copy_lock);
    if (nzombies) zombie_sweep(replayed);
    for (int i = 0; i < n; i++) {
        copy_prefix *pf = copy_find(handles[i]);
        if (!pf) {
            plat_mutex_unlock(&copy_lock);
            rt_fatal("nvn: QueueSubmitCommands: command handle %#llx is not a live recording (released or never recorded)",
                     (unsigned long long)handles[i]);
        }
        out[i] = handles[i];
        if (pf->used_at > replayed && pf->used_at != now && pf->deferred != 1) {
            size_t sz = sizeof *pf + pf->bytes;
            copy_prefix *cl = malloc(sz);
            if (!cl) rt_fatal("nvn: out of memory (recording clone, %zu bytes)", sz);
            memcpy(cl, pf, sz);
            cl->magic = 0;   // not a live handle: the game never sees it
            cl->used_at = now;
            zombie_add(cl);
            out[i] = (uintptr_t)(cl + 1);
            __atomic_fetch_add(&n_clones, 1, __ATOMIC_RELAXED);
        } else {
            pf->used_at = now;
        }
    }
    plat_mutex_unlock(&copy_lock);
    for (int i = 0; i < n; i++) {
        int nd = resolve_deferred((const uint8_t *)(uintptr_t)out[i], 0);
        if (out[i] == handles[i]) ((copy_prefix *)(uintptr_t)out[i] - 1)->deferred = nd ? 2 : 1;
    }
    if (now % 7200 == 0)
        rt_log("nvn: asynchronous replay at submit %llu: %llu recording clones so far, %u copies waiting for the replay",
               (unsigned long long)now, (unsigned long long)__atomic_load_n(&n_clones, __ATOMIC_RELAXED), nzombies);
}

uint64_t nvn_submit_count(void) { return __atomic_load_n(&nsubmits, __ATOMIC_RELAXED); }

// A recording made of ops [ops, ops + n) (no OP_BEGIN or OP_END among them) for the replay alone (nvn_qorder.c's
// streams): freed once the replay reached the newest submission, as the clones are.
uint64_t nvn_stream_handle(const uint8_t *ops, size_t n) {
    copy_prefix *pf = malloc(sizeof *pf + n + 16);
    if (!pf) rt_fatal("nvn: out of memory (queue order stream, %zu bytes)", n);
    uint8_t *p = (uint8_t *)(pf + 1);
    put_hdr(p, OP_BEGIN, 8);
    memcpy(p + 8, ops, n);
    put_hdr(p + 8 + n, OP_END, 8);
    plat_mutex_lock(&copy_lock);
    *pf = (copy_prefix){0, n + 16, nsubmits, nsubmits, 1};
    zombie_add(pf);
    plat_mutex_unlock(&copy_lock);
    return (uintptr_t)p;
}

const char *nvn_op_name(uint32_t op) {
    static const char *const names[] = {
        "END", "JUMP", "BEGIN", "CALL", "SIGNAL_EVENT", "REPORT_COUNTER", "COPY_B2B", "UPDATE_UB", "SET_RT",
        "CLEAR_COLOR", "CLEAR_DS", "VIEWPORTS", "SCISSOR", "DEPTH_RANGE", "BIND_PROGRAM", "BIND_STATE", "BIND_VATTR",
        "BIND_VSTREAM", "BIND_VBUF", "BIND_UBO", "BIND_SSBO", "BIND_TEX", "BIND_SAMP", "STENCIL", "POLY_OFFSET", "DRAW",
        "DRAW_IDX", "DISPATCH", "BARRIER", "SET_TEX_POOL", "SET_SAMP_POOL", "COPY_B2T", "COPY_STRIDE", "BLEND_COLOR",
        "COPY_T2B", "BIND_IMG", "COPY_T2T", "RESET_COUNTER", "CLEAR_TEX", "DEPTH_CLAMP", "RENDER_ENABLE",
        "RASTER_DISCARD", "RENDER_COND", "PRIM_RESTART", "DRAW_IDX_IND", "DISPATCH_IND", "CLEAR_BUF", "DRAW_MESH", "ALPHA_REF",
        "WAIT_EVENT", "QUEUE", "PUSH_CONST",
    };
    _Static_assert(sizeof names / sizeof *names == OP_COUNT_, "one name per op");
    return op < OP_COUNT_ ? names[op] : "?";
}
