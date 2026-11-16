// MKW_CAPTURE=1: the back end is wrapped from the boot so the state a capture starts from is known (live pools in init
// order, the MapVirtual requests of the live virtual pools, the texture and sampler slots). A capture starts at a
// present asked for with MKW_CAPTURE_AT=<present> or the control channel (`capture DIR [FRAMES]`, ctl.c); it drains the
// queue, writes the guest memory and that state, then every back end call that changes what the GPU sees until FRAMES
// presents later (MKW_CAPTURE_FRAMES, default 2), each submit and present preceded by the guest pages written since the
// last record. The run then ends (MKW_CAPTURE_EXIT=0 keeps it going). Without MKW_CAPTURE nothing is wrapped.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cap.h"
#include "cfg.h"
#include "host.h"
#include "nvn_api.h"
#include "nvn_tex.h"
#include "platform/platform.h"
#include "vk.h"

static const GfxBackend *in;   // the wrapped back end
static PlatMutex lock = PLAT_MUTEX_INIT;

// ---- state since the boot (lock)
typedef struct { pool_t *pool; int n; uint8_t *req; } MapV;
typedef struct { int used, has_view; tex_t t; view_t v; } TexSlot;
typedef struct { int used; samp_par p; } SampSlot;
static pool_t **pools;
static int npools, cap_pools;
static MapV *mapv;
static int nmapv, cap_mapv;
static TexSlot *tex;
static SampSlot *samp;
static int ntex, nsamp;

static void *grow(void *p, int *cap, int need, size_t elem) {
    if (need <= *cap) return p;
    int n = *cap ? *cap : 256;
    while (n < need) n *= 2;
    uint8_t *q = realloc(p, elem * (size_t)n);
    if (!q) rt_fatal("cap: out of memory for the back end state");
    memset(q + elem * (size_t)*cap, 0, elem * (size_t)(n - *cap));
    *cap = n;
    return q;
}

// ---- the capture in progress (lock)
static CapOut *out;
static char want_dir[400];
static int want_frames;
static uint64_t want_at = UINT64_MAX, presents, first, frames_left;

// Without the lock: the control commands of MKW_SEQ_CTL run inside the back end's present (perf_present), while
// w_present holds it. The present after the next one picks the request up (maybe_start).
static char req_dir[400];
static volatile int req_frames;
int cap_request(const char *dir, int frames) {
    if (!in || __atomic_load_n(&req_frames, __ATOMIC_ACQUIRE)) return 0;
    snprintf(req_dir, sizeof req_dir, "%s", dir);
    __atomic_store_n(&req_frames, frames > 0 ? frames : 2, __ATOMIC_RELEASE);
    return 1;
}

static void write_state(void) {
    for (int i = 0; i < npools; i++) {
        uint64_t a = (uintptr_t)pools[i];
        cap_out(out, CAP_POOL_INIT, &a, sizeof a, NULL, 0);
    }
    for (int i = 0; i < nmapv; i++) {
        uint64_t h[2] = {(uintptr_t)mapv[i].pool, (uint64_t)mapv[i].n};
        cap_out(out, CAP_MAPV, h, sizeof h, mapv[i].req, (size_t)mapv[i].n * 0x28);
    }
    for (int i = 0; i < ntex; i++)
        if (tex[i].used) {
            CapTexReg r = {i, tex[i].has_view, tex[i].t, tex[i].v};
            cap_out(out, CAP_TEXREG, &r, sizeof r, NULL, 0);
        }
    for (int i = 0; i < nsamp; i++)
        if (samp[i].used) {
            int32_t id = i;
            cap_out(out, CAP_SAMPREG, &id, sizeof id, &samp[i].p, sizeof samp[i].p);
        }
}

static int by_mem(const void *a, const void *b) {
    const tex_t *s = &((const TexSlot *)a)->t, *u = &((const TexSlot *)b)->t;
    uintptr_t x = (uintptr_t)s->mem, y = (uintptr_t)u->mem;
    return x != y ? (x < y ? -1 : 1) : memcmp(&s->d, &u->d, sizeof s->d);
}

// What only the GPU holds: the images of registered textures the GPU wrote (a colour grading LUT rendered once at the
// race's load: cap-race3's replay drew a black frame without it). Read level by level and written as CAP_WTEX, which
// the replay gives to write_texels. Compressed formats are never render targets and are left out.
#define RECENT 64   // submissions: ~12 frames of a race (5 a frame)
static uint64_t write_gpu_textures(int *count) {
    TexSlot *v = malloc(sizeof *v * (size_t)(ntex + 1));
    if (!v) rt_fatal("cap: out of memory");
    int n = 0;
    for (int i = 0; i < ntex; i++)
        if (tex[i].used && tex[i].t.mem) v[n++] = tex[i];
    qsort(v, (size_t)n, sizeof *v, by_mem);
    uint64_t bytes = 0;
    size_t cap = 0;
    uint8_t *buf = NULL;
    *count = 0;
    for (int i = 0; i < n; i++) {
        const tex_t *t = &v[i].t;
        // One image per storage and descriptor: slots repeat them (views, registrations again), but another descriptor
        // at the same storage is another image (cap-race8's race replayed dark with one per address).
        if (i && v[i - 1].t.mem == t->mem && !memcmp(&v[i - 1].t.d, &t->d, sizeof t->d)) continue;
        // Every image the CPU cannot give back (the GPU wrote it, or its storage is GPU-only memory: copies into it,
        // sparse tiles; cap-race8's lighting volumes and track textures replayed as zeros without them) that a draw
        // sampled in the last RECENT submissions: cap-race9 read 21.8 GB, 98 % of it six sparse 8192x8192x68 arrays.
        int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh), st = bpb ? vk_texture_capture_state(t, RECENT) : 0;
        if (!(st & VK_TEXCAP_RECENT) || (!(st & VK_TEXCAP_GPU) && !nvn_gpu_only((uintptr_t)t->mem))) continue;
        // A Vulkan sparse image's tiles go as CAP_SPARSE; a sparse NVN texture made whole (depth in RADV: the shadow
        // map array, cap-race10's first pass to differ from the game) goes here.
        if (st & VK_TEXCAP_SPARSE) continue;
        // An ASTC texture decoded into another format has no blocks for ReadTexels to give (it answers zeros, and the
        // replay decoded those as the error colour: tt4's course banner, an array filled by texture copies, replayed
        // flat magenta): its image goes as it is, in slices of layers of at most SLICE bytes like the texels below.
        VkTexDecoded dec;
        if (vk_texture_decoded(t, &dec)) {
            const uint64_t SLICE = 16ull << 20;
            for (uint32_t l = 0; l < dec.levels; l++) {
                uint32_t nz = dec.bytes[l] >= SLICE ? 1 : (uint32_t)(SLICE / dec.bytes[l]);
                for (uint32_t z = 0; z < dec.layers; z += nz) {
                    CapWimg w;
                    memset(&w, 0, sizeof w);
                    w.t = *t, w.format = (uint32_t)dec.format, w.level = l, w.layer = z;
                    w.nlayers = dec.layers - z < nz ? dec.layers - z : nz;
                    w.bytes = dec.bytes[l] * w.nlayers;
                    if (w.bytes > cap && !(buf = realloc(buf, cap = w.bytes))) rt_fatal("cap: out of memory for texels");
                    if (!vk_texture_decoded_copy(t, dec.format, l, z, w.nlayers, buf, w.bytes, 0)) continue;
                    cap_out(out, CAP_WIMG, &w, sizeof w, buf, w.bytes);
                    bytes += w.bytes;
                }
            }
            ++*count;
            continue;
        }
        int is3d = t->d.target == NVN_TEXTURE_TARGET_3D;
        uint64_t tex_bytes = 0;
        for (uint32_t l = 0; l < (t->d.levels ? t->d.levels : 1); l++) {
            uint32_t wd = t->d.width >> l, ht = t->d.height >> l, dp = is3d ? t->d.depth >> l : t->d.depth;
            wd = wd ? wd : 1, ht = ht ? ht : 1, dp = dp ? dp : 1;
            uint32_t bx = (wd + (uint32_t)bw - 1) / (uint32_t)bw, by = (ht + (uint32_t)bh - 1) / (uint32_t)bh;   // blocks
            // Slices of at most SLICE bytes (layers, then block rows): ReadTexels stages the whole region at once, and a
            // 256x256x401 array took 288 MiB of staging at the title (cap-race4) and failed under MKW_GPU_MEM_LIMIT.
            const uint64_t SLICE = 16ull << 20, row = (uint64_t)bpb * bx, layer = row * by;
            uint32_t nz = layer >= SLICE ? 1 : (uint32_t)(SLICE / layer),
                     ny = (layer >= SLICE ? (uint32_t)(SLICE / row) : by) * (uint32_t)bh;   // in texels, whole blocks
            for (uint32_t z = 0; z < dp; z += nz)
                for (uint32_t y = 0; y < ht; y += ny) {
                    CapWtex w;
                    memset(&w, 0, sizeof w);
                    w.t = *t;
                    w.has_view = 1;
                    w.v.set_levels = 1, w.v.base_level = (uint8_t)l, w.v.nlevels = 1;
                    w.r = (NVNcopyRegion){0, (int)y, (int)z, (int)wd, (int)(ht - y < ny ? ht - y : ny),
                                          (int)(dp - z < nz ? dp - z : nz)};
                    w.row = row, w.img = row * (((uint64_t)w.r.height + (uint64_t)bh - 1) / (uint64_t)bh);
                    w.bytes = vk_texture_texel_span(t, &w.v, &w.r, w.row, w.img);
                    if (!w.bytes) continue;
                    if (w.bytes > cap && !(buf = realloc(buf, cap = w.bytes))) rt_fatal("cap: out of memory for texels");
                    in->read_texels(t, &w.v, &w.r, buf, w.row, w.img);
                    cap_out(out, CAP_WTEX, &w, sizeof w, buf, w.bytes);
                    bytes += w.bytes;
                    tex_bytes += w.bytes;
                }
        }
        if (tex_bytes >= (64ull << 20))
            rt_log("cap: GPU-written texture %p %ux%ux%u fmt %#x levels %u target %d: %.0f MiB", (void *)t->mem,
                   t->d.width, t->d.height, t->d.depth, t->d.format, t->d.levels, t->d.target,
                   (double)tex_bytes / (1 << 20));
        ++*count;
    }
    free(buf);
    free(v);
    return bytes;
}

static void emit_slot(const void *key, uint32_t klen, const void *p, uint64_t n, void *ctx) {
    (void)ctx;
    uint64_t h[2] = {klen, n};
    CapHdr hd = {CAP_SPARSE, 0, sizeof h + klen + n};
    cap_out_raw(out, &hd, sizeof hd);
    cap_out_raw(out, h, sizeof h);
    cap_out_raw(out, key, klen);
    cap_out_raw(out, p, n);
}

static void emit_chunk(uint64_t addr, const void *p, uint64_t n, void *ctx) {
    (void)ctx;
    uint64_t h[2] = {addr, n};
    cap_out(out, CAP_GPUMEM, h, sizeof h, p, n);
}

// At a present (lock held, the present already handed on): starts the capture asked for at this one.
static void maybe_start(void) {
    int rq = __atomic_load_n(&req_frames, __ATOMIC_ACQUIRE);
    if (rq && !out) {
        snprintf(want_dir, sizeof want_dir, "%s", req_dir);
        want_frames = rq, want_at = presents + 1;
        __atomic_store_n(&req_frames, 0, __ATOMIC_RELEASE);
    }
    if (out || presents + 1 < want_at) return;
    char path[480];
    plat_file_mkdir_p(want_dir);
    snprintf(path, sizeof path, "%s/ref", want_dir);
    plat_file_mkdir_p(path);
    snprintf(path, sizeof path, "%s/capture.zst", want_dir);
    uint64_t t0 = plat_now_ns();
    in->finish();   // the replay and the GPU are done with everything before: memory holds what they wrote
    out = cap_out_open(path);
    const RtGuestLayout *L = mem_layout();
    CapMeta m = {CAP_MAGIC, CAP_VERSION, (uint32_t)nvn_depth_minus_w(), presents + 1, (uint64_t)want_frames,
                 L->heap.base, 0, L->alias.base, L->stack.base};
    cap_out(out, CAP_META, &m, sizeof m, NULL, 0);
    if (!cap_mem_snapshot(out)) rt_fatal("cap: this platform cannot list populated guest pages");
    uint64_t mem = cap_out_bytes(out);
    write_state();
    int ngpu = 0;
    uint64_t gpu_bytes = write_gpu_textures(&ngpu), chunk_bytes = vk_mem_phys_dump(emit_chunk, NULL),
             sparse_bytes = vk_sparse_dump(emit_slot, NULL);
    first = presents + 1, frames_left = (uint64_t)want_frames;
    want_at = UINT64_MAX;
    rt_log("cap: capture of %llu frames from present %llu into %s: snapshot %.0f MiB, %d pools, %d MapVirtual calls, "
           "%d GPU-written textures %.0f MiB, physical pool chunks %.0f MiB, sparse tiles %.0f MiB, %.1f s", (unsigned long long)frames_left,
           (unsigned long long)first, path, (double)mem / (1 << 20), npools, nmapv, ngpu, (double)gpu_bytes / (1 << 20),
           (double)chunk_bytes / (1 << 20), (double)sparse_bytes / (1 << 20), (double)(plat_now_ns() - t0) / 1e9);
}

// The pages written since the last record. Only before a submit and a present: the back end reads guest memory when
// it replays, and a pass over the ~4 GiB a race holds costs ~0.3 s (guest_written alone comes thousands of times).
static void delta(void) {
    if (out) cap_mem_delta(out);
}

int cap_busy(void) {
    return in && (__atomic_load_n(&out, __ATOMIC_ACQUIRE) || __atomic_load_n(&req_frames, __ATOMIC_ACQUIRE) ||
                  __atomic_load_n(&want_at, __ATOMIC_ACQUIRE) != UINT64_MAX);
}

// ---- the wrapped calls
static int w_pool_init(pool_t *p) {
    int ok = in->pool_init(p);
    plat_mutex_lock(&lock);
    if (ok) {
        pools = grow(pools, &cap_pools, npools + 1, sizeof *pools);
        pools[npools++] = p;
        if (out) {
            // The pool object first: the replay reads its storage and size from guest memory, and the game wrote it in
            // this frame, after the last memory delta (#26: a pool made at frame 4102 read mem 0 and committed page 0).
            uint64_t a = (uintptr_t)p, m[2] = {a, sizeof *p};
            cap_out(out, CAP_MEM, m, sizeof m, p, sizeof *p);
            cap_out(out, CAP_POOL_INIT, &a, sizeof a, NULL, 0);
        }
    }
    plat_mutex_unlock(&lock);
    return ok;
}

static void w_pool_fini(pool_t *p) {
    plat_mutex_lock(&lock);
    for (int i = 0; i < npools; i++)
        if (pools[i] == p) memmove(&pools[i], &pools[i + 1], sizeof *pools * (size_t)(--npools - i)), i--;
    for (int i = 0; i < nmapv; i++)
        if (mapv[i].pool == p) free(mapv[i].req), memmove(&mapv[i], &mapv[i + 1], sizeof *mapv * (size_t)(--nmapv - i)), i--;
    if (out) {
        uint64_t a = (uintptr_t)p;
        cap_out(out, CAP_POOL_FINI, &a, sizeof a, NULL, 0);
    }
    plat_mutex_unlock(&lock);
    in->pool_fini(p);
}

static int w_pool_map_virtual(pool_t *v, int n, const void *requests) {
    plat_mutex_lock(&lock);
    mapv = grow(mapv, &cap_mapv, nmapv + 1, sizeof *mapv);
    MapV *m = &mapv[nmapv++];
    m->pool = v, m->n = n, m->req = malloc((size_t)n * 0x28 + 1);
    if (!m->req) rt_fatal("cap: out of memory for MapVirtual requests");
    memcpy(m->req, requests, (size_t)n * 0x28);
    if (out) {
        uint64_t h[2] = {(uintptr_t)v, (uint64_t)n};
        cap_out(out, CAP_MAPV, h, sizeof h, requests, (size_t)n * 0x28);
    }
    plat_mutex_unlock(&lock);
    return in->pool_map_virtual(v, n, requests);
}

static void w_texture_register(int id, const tex_t *t, const view_t *v) {
    plat_mutex_lock(&lock);
    if (id >= 0 && id < (1 << 20)) {
        tex = grow(tex, &ntex, id + 1, sizeof *tex);
        tex[id] = (TexSlot){1, v != NULL, *t, {0}};
        if (v) tex[id].v = *v;
        if (out) {
            CapTexReg r = {id, v != NULL, *t, tex[id].v};
            cap_out(out, CAP_TEXREG, &r, sizeof r, NULL, 0);
        }
    }
    plat_mutex_unlock(&lock);
    in->texture_register(id, t, v);
}

static void w_sampler_register(int id, const samp_par *s) {
    plat_mutex_lock(&lock);
    if (id >= 0 && id < (1 << 20)) {
        samp = grow(samp, &nsamp, id + 1, sizeof *samp);
        samp[id] = (SampSlot){1, *s};
        if (out) {
            int32_t i32 = id;
            cap_out(out, CAP_SAMPREG, &i32, sizeof i32, s, sizeof *s);
        }
    }
    plat_mutex_unlock(&lock);
    in->sampler_register(id, s);
}

static void w_texture_release(const tex_t *t) {
    plat_mutex_lock(&lock);
    if (out && t) cap_out(out, CAP_TEXREL, t, sizeof *t, NULL, 0);
    plat_mutex_unlock(&lock);
    in->texture_release(t);
}

static void w_program_release(const prog_t *p) {
    plat_mutex_lock(&lock);
    if (out) {
        uint64_t a = (uintptr_t)p;
        cap_out(out, CAP_PROGREL, &a, sizeof a, NULL, 0);
    }
    plat_mutex_unlock(&lock);
    in->program_release(p);
}

// The pinned tex_t pointers of a payload (host heap: written out by value).
static int tex_fields(const op_hdr *h, uint32_t off[9]) {
    const uint8_t *p = (const uint8_t *)nvn_op_payload(h);
    uint32_t base = (uint32_t)sizeof *h;
    int n = 0;
    switch (h->op) {
    case OP_SET_RT:
        for (int i = 0; i < 8; i++)
            if (((const p_set_rt *)p)->color[i]) off[n++] = base + (uint32_t)offsetof(p_set_rt, color[i]);
        if (((const p_set_rt *)p)->depth) off[n++] = base + (uint32_t)offsetof(p_set_rt, depth);
        break;
    case OP_COPY_B2T:
    case OP_COPY_T2B:
        if (((const p_copy_b2t *)p)->tex) off[n++] = base + (uint32_t)offsetof(p_copy_b2t, tex);
        break;
    case OP_COPY_T2T:
        if (((const p_copy_t2t *)p)->src) off[n++] = base + (uint32_t)offsetof(p_copy_t2t, src);
        if (((const p_copy_t2t *)p)->dst) off[n++] = base + (uint32_t)offsetof(p_copy_t2t, dst);
        break;
    case OP_CLEAR_TEX:
        if (((const p_clear_tex *)p)->tex) off[n++] = base + (uint32_t)offsetof(p_clear_tex, tex);
        break;
    }
    return n;
}

static void write_handle(uint64_t handle) {
    const op_hdr *h0 = nvn_op_first(handle), *h = h0;
    static uint32_t *offs;
    static const tex_t **ptrs;
    static int cap_offs, cap_ptrs;
    int nfix = 0;
    for (;; h = nvn_op_next(h)) {
        uint32_t off[9];
        int k = tex_fields(h, off);
        offs = grow(offs, &cap_offs, nfix + k + 1, sizeof *offs);
        ptrs = grow(ptrs, &cap_ptrs, nfix + k + 1, sizeof *ptrs);
        for (int i = 0; i < k; i++) {
            uint32_t at = (uint32_t)((const uint8_t *)h - (const uint8_t *)h0) + off[i];
            offs[nfix] = at;
            memcpy(&ptrs[nfix], (const uint8_t *)h0 + at, sizeof *ptrs);
            nfix++;
        }
        if (h->op == OP_END) break;
    }
    uint64_t bytes = (uint64_t)((const uint8_t *)nvn_op_next(h) - (const uint8_t *)h0);
    uint64_t hd[2] = {bytes, (uint64_t)nfix};
    cap_out_raw(out, hd, sizeof hd);
    cap_out_raw(out, h0, bytes);
    cap_out_raw(out, offs, sizeof *offs * (size_t)nfix);
    for (int i = 0; i < nfix; i++) cap_out_raw(out, ptrs[i], sizeof(tex_t));
}

static uint64_t handle_record_bytes(uint64_t handle) {
    const op_hdr *h0 = nvn_op_first(handle), *h = h0;
    uint64_t nfix = 0;
    for (;; h = nvn_op_next(h)) {
        uint32_t off[9];
        nfix += (uint64_t)tex_fields(h, off);
        if (h->op == OP_END) break;
    }
    uint64_t bytes = (uint64_t)((const uint8_t *)nvn_op_next(h) - (const uint8_t *)h0);
    return 16 + bytes + nfix * (4 + sizeof(tex_t));
}

static void record_submit(const uint64_t *handles, int n) {   // lock held
    if (out) {
        delta();
        uint64_t size = 8;
        for (int i = 0; i < n; i++) size += handle_record_bytes(handles[i]);
        int64_t nn = n;
        CapHdr hd = {CAP_SUBMIT, 0, size};
        cap_out_raw(out, &hd, sizeof hd);
        cap_out_raw(out, &nn, sizeof nn);
        for (int i = 0; i < n; i++) write_handle(handles[i]);
    }
}

static uint64_t w_submit(const uint64_t *handles, int n) {
    plat_mutex_lock(&lock);
    record_submit(handles, n);
    uint64_t f = in->submit(handles, n);
    plat_mutex_unlock(&lock);
    return f;
}

// A held submission (nvn_qorder.c) goes into the capture when its handles come: the capture replays it in that place,
// after what was recorded meanwhile, while the game's replay keeps the place it was queued in.
static void w_held_fill(void *slot, const uint64_t *handles, int n) {
    plat_mutex_lock(&lock);
    record_submit(handles, n);
    in->held_fill(slot, handles, n);
    plat_mutex_unlock(&lock);
}

static void w_present(const tex_t *t, const int crop[4], uint64_t at_ns) {
    plat_mutex_lock(&lock);
    if (out) {
        delta();
        CapPresent p = {*t, crop != NULL, {0}};
        if (crop) memcpy(p.crop, crop, sizeof p.crop);
        cap_out(out, CAP_PRESENT, &p, sizeof p, NULL, 0);
        // The game's own image of this frame (boot.c saves it once the present is out): the reference of a replay.
        char ref[480];
        snprintf(ref, sizeof ref, "%s/ref/f-f%llu.png", want_dir, (unsigned long long)(presents + 1));
        rt_shot_request(ref);
    }
    in->present(t, crop, at_ns);
    presents++;
    if (out) {
        cap_out(out, CAP_FRAME, &presents, sizeof presents, NULL, 0);
        if (!--frames_left) {
            uint64_t bytes = cap_out_bytes(out);
            cap_out_close(out);
            out = NULL;
            rt_log("cap: capture done at present %llu: %.0f MiB of records", (unsigned long long)presents,
                   (double)bytes / (1 << 20));
            const char *e = cfg_get("MKW_CAPTURE_EXIT");
            if (!e || strcmp(e, "0")) {
                plat_mutex_unlock(&lock);
                rt_log("cap: run ends (MKW_CAPTURE_EXIT=0 keeps it going)");
                vk_pipeline_save();
                plat_exit(0);
            }
        }
    }
    maybe_start();
    plat_mutex_unlock(&lock);
}

static void w_finish(void) {
    plat_mutex_lock(&lock);
    if (out) cap_out(out, CAP_FINISH, NULL, 0, NULL, 0);
    plat_mutex_unlock(&lock);
    in->finish();
}

static void w_write_texels(const tex_t *t, const view_t *v, const NVNcopyRegion *r, const uint8_t *src, uint64_t row,
                           uint64_t img) {
    plat_mutex_lock(&lock);
    if (out) {
        CapWtex w = {*t, v != NULL, 0, {0}, *r, row, img, vk_texture_texel_span(t, v, r, row, img)};
        if (v) w.v = *v;
        cap_out(out, CAP_WTEX, &w, sizeof w, src, w.bytes);
    }
    plat_mutex_unlock(&lock);
    in->write_texels(t, v, r, src, row, img);
}

static void w_guest_written(uint64_t addr, uint64_t size) {
    plat_mutex_lock(&lock);
    if (out) {
        uint64_t a[2] = {addr, size};
        cap_out(out, CAP_GWRITTEN, a, sizeof a, NULL, 0);
    }
    plat_mutex_unlock(&lock);
    in->guest_written(addr, size);
}

static GfxBackend wrapped;

const GfxBackend *cap_wrap(const GfxBackend *b) {
    const char *e = cfg_get("MKW_CAPTURE");
    if (!e || !strcmp(e, "0") || b != &gfx_vulkan_backend) return b;
    in = b;
    wrapped = *b;
    wrapped.name = "vulkan (captured)";
    wrapped.pool_init = w_pool_init, wrapped.pool_fini = w_pool_fini, wrapped.pool_map_virtual = w_pool_map_virtual;
    wrapped.texture_register = w_texture_register, wrapped.sampler_register = w_sampler_register;
    wrapped.texture_release = w_texture_release, wrapped.program_release = w_program_release;
    wrapped.submit = w_submit, wrapped.present = w_present, wrapped.finish = w_finish;
    if (b->held_fill) wrapped.held_fill = w_held_fill;
    wrapped.write_texels = w_write_texels, wrapped.guest_written = w_guest_written;
    const char *at = cfg_get("MKW_CAPTURE_AT"), *dir = cfg_get("MKW_CAPTURE_DIR"), *fr = cfg_get("MKW_CAPTURE_FRAMES");
    snprintf(want_dir, sizeof want_dir, "%s", dir && *dir ? dir : "build/cap/capture");
    want_frames = fr && atoi(fr) > 0 ? atoi(fr) : 2;
    if (at && *at) want_at = strtoull(at, NULL, 0);
    rt_log("cap: back end wrapped for a capture (MKW_CAPTURE); %s", at && *at ? "starts at MKW_CAPTURE_AT" : "waits for ctl capture");
    return &wrapped;
}
