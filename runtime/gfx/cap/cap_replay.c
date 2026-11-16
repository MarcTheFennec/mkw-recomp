// mkw --replay DIR [--out DIR] [--tag NAME]: a capture (cap.h) through the Vulkan back end, without the game. The guest
// memory goes back to its addresses, the back end gets its pools, mappings and slots again, then every recorded call in
// order; each present is read back and written as <out>/<tag>-f<present>.png. Synchronous replay and pipelines built
// at the draw (MKW_REPLAY_ASYNC=0, MKW_PIPE_SYNC=1 unless set) so two replays of one capture differ only in what they
// were asked to change: MKW_REPLAY_PAR=1 against 0 compares the chunked recording with the serial one.
// The summary line: submits, their replay time (recording and submit on this thread), validation errors.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cap.h"
#include "cfg.h"
#include "host.h"
#include "nvn_api.h"
#include "platform/platform.h"
#include "vk.h"
#include "vk_rec.h"
#include "vk_exec.h"
#include "vk_reuse.h"

// One tex_t copy per serial, as the front end's pins (nvn_api_cmd.c cmd_tex_pin): the payloads of a run keep pointing
// at the same copy.
static tex_t **pins;
static uint64_t npins, cap_pins;   // open addressing on the serial; cap_pins a power of two

static const tex_t *pin(const tex_t *t) {
    if (npins * 2 >= cap_pins) {
        uint64_t old = cap_pins;
        tex_t **o = pins;
        cap_pins = cap_pins ? cap_pins * 2 : 4096;
        if (!(pins = calloc(cap_pins, sizeof *pins))) rt_fatal("cap: out of memory for texture pins");
        npins = 0;
        for (uint64_t i = 0; i < old; i++)
            if (o[i]) {
                uint64_t k = o[i]->serial * 0x9e3779b97f4a7c15ull & (cap_pins - 1);
                while (pins[k]) k = (k + 1) & (cap_pins - 1);
                pins[k] = o[i], npins++;
            }
        free(o);
    }
    uint64_t k = t->serial * 0x9e3779b97f4a7c15ull & (cap_pins - 1);
    for (; pins[k]; k = (k + 1) & (cap_pins - 1))
        if (pins[k]->serial == t->serial) return pins[k];
    if (!(pins[k] = malloc(sizeof *t))) rt_fatal("cap: out of memory for texture pins");
    *pins[k] = *t;
    npins++;
    return pins[k];
}

static void save_png(const GfxBackend *b, const tex_t *t, const char *path) {
    int fmt = t->d.format;
    int bgra = fmt == NVN_FORMAT_BGRA8 || fmt == NVN_FORMAT_BGRA8_SRGB;
    int rgb10 = fmt == NVN_FORMAT_RGB10A2;
    if (!bgra && !rgb10 && fmt != NVN_FORMAT_RGBA8 && fmt != NVN_FORMAT_RGBA8_SRGB) {
        rt_log("cap: present of NVN format %#x not saved (8-bit RGBA, BGRA and RGB10A2 only)", fmt);
        return;
    }
    uint32_t w = t->d.width, h = t->d.height;
    uint8_t *px = malloc(4ull * w * h);
    if (!px) rt_fatal("cap: out of memory for the image");
    view_t view;
    memset(&view, 0, sizeof view);
    NVNcopyRegion r = {.width = (int)w, .height = (int)h, .depth = 1};
    b->finish();
    b->read_texels(t, &view, &r, px, 4ull * w, 4ull * w * h);
    if (rgb10)
        // Game 1.8.0 presents RGB10A2 (A2B10G10R10, R in the low bits); PNG is 8-bit, so each channel keeps its top 8.
        for (uint64_t i = 0; i < 4ull * w * h; i += 4) {
            uint32_t v;
            memcpy(&v, px + i, 4);
            px[i] = (v >> 2) & 0xff, px[i + 1] = (v >> 12) & 0xff, px[i + 2] = (v >> 22) & 0xff;
        }
    else if (bgra)
        for (uint64_t i = 0; i < 4ull * w * h; i += 4) {
            uint8_t x = px[i];
            px[i] = px[i + 2], px[i + 2] = x;
        }
    for (uint64_t i = 3; i < 4ull * w * h; i += 4) px[i] = 255;   // the presented alpha is not what the screen shows
    rt_write_png(path, px, w, h);
    free(px);
}

static int usage(void) {
    fprintf(stderr, "usage: mkw --replay DIR [--out DIR] [--tag NAME]\n");
    return 2;
}

static void env_default(const char *name, const char *value) {
    if (!getenv(name)) plat_setenv(name, value);
}

int cap_replay_main(int argc, char **argv) {
    if (argc < 1) return usage();
    const char *dir = argv[0], *outdir = NULL, *tag = NULL;
    for (int i = 1; i < argc; i++) {
        if (i + 1 < argc && !strcmp(argv[i], "--out")) outdir = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "--tag")) tag = argv[++i];
        else return usage();
    }
    env_default("MKW_REPLAY_ASYNC", "0");
    env_default("MKW_PIPE_SYNC", "1");
    env_default("MKW_REPLAY_PAR_WARM", "0");   // chunks from the first submission (vk_exec_par.c)
    env_default("MKW_BACKGROUND_WINDOW", "hidden");   // a replay shows no window: its images are read back
    vk_pipeline_async_enable();   // MKW_PIPE_SYNC=0: background builds and the draws' waits, as in the game
    const char *par = getenv("MKW_REPLAY_PAR");
    if (!tag) tag = par && strcmp(par, "0") ? "par" : "serial";
    char path[1024], out_default[1024];
    snprintf(path, sizeof path, "%s/capture.zst", dir);
    if (!outdir) snprintf(out_default, sizeof out_default, "%s/out", dir), outdir = out_default;
    plat_file_mkdir_p(outdir);
    CapIn *in = cap_in_open(path);
    if (!in) return fprintf(stderr, "replay: cannot open %s\n", path), 1;

    const GfxBackend *b = &gfx_vulkan_backend;
    CapHdr h;
    CapMeta m;
    if (!cap_in_next(in, &h) || h.type != CAP_META || h.size < sizeof m) rt_fatal("cap: %s has no header", path);
    cap_in_read(in, &m, sizeof m);
    const RtGuestLayout *L = mem_layout();
    if (m.magic != CAP_MAGIC || m.version != CAP_VERSION) rt_fatal("cap: %s is not a version %u capture", path, CAP_VERSION);
    if (m.heap_base != L->heap.base || m.alias_base != L->alias.base || m.stack_base != L->stack.base)
        rt_fatal("cap: guest layout differs from the capture's (heap %#llx here, %#llx there)",
                 (unsigned long long)L->heap.base, (unsigned long long)m.heap_base);
    // DeviceSetDepthMode runs once at the boot: without it a z = -w full-screen quad (the final upscale) is clipped
    // and the frame is black (the title and race captures, 07/11/2026).
    nvn_depth_minus_w_set((int)m.depth_minus_w);
    rt_log("cap: replay of %s: %llu frames from present %llu, tag %s", dir, (unsigned long long)m.frames,
           (unsigned long long)m.first_frame, tag);

    uint64_t t_start = plat_now_ns(), t_state = 0, mem_bytes = 0, submits = 0, sub_ns = 0, presents = 0, frame = 0;
    uint64_t frame_sub0 = 0;   // sub_ns at the last present
    // The thread's CPU time in the submits: their wall time also holds the waits for the GPU, which in a replay is busier
    // than the CPU (a 91 % shorter recording left the wall time as it was, 08/11/2026, grabacion-reutilizada).
    uint64_t sub_cpu = 0, frame_cpu0 = 0;
    size_t bufcap = 0;
    uint8_t *buf = NULL;
    // Time per record type, from its start to the next one's (reading and decompressing included): what a replay's
    // 7-22 s of state are made of.
    uint64_t type_ns[CAP_LAST + 1] = {0}, type_n[CAP_LAST + 1] = {0}, t_rec = plat_now_ns();
    uint32_t prev = 0, wimg_skipped = 0;
    for (int seen_submit = 0; cap_in_next(in, &h);) {
        uint64_t now = plat_now_ns();
        if (prev) type_ns[prev] += now - t_rec;
        prev = h.type <= CAP_LAST ? h.type : 0, t_rec = now;
        if (prev) type_n[prev]++;
        if (h.size > bufcap) {
            bufcap = h.size;
            if (!(buf = realloc(buf, bufcap))) rt_fatal("cap: out of memory for a %llu-byte record", (unsigned long long)h.size);
        }
        switch (h.type) {
        case CAP_MEM: {
            uint64_t a[2];
            cap_in_read(in, a, sizeof a);
            cap_in_read(in, buf, a[1]);
            cap_mem_restore(buf, a[0], a[1]);
            mem_bytes += a[1];
            break;
        }
        case CAP_ZERO: {
            uint64_t a[2];
            cap_in_read(in, a, sizeof a);
            cap_mem_zero(a[0], a[1]);
            break;
        }
        case CAP_POOL_INIT:
        case CAP_POOL_FINI:
        case CAP_PROGREL: {
            uint64_t a;
            cap_in_read(in, &a, sizeof a);
            if (h.type == CAP_POOL_INIT) {
                // The capture holds the populated pages only; the rest of a pool's storage was committed and zero in
                // the game, and the back end reads all of it (shadow copies).
                const pool_t *p = (const pool_t *)(uintptr_t)a;
                uint64_t lo = (uintptr_t)p->mem & ~0xfffull, hi = ((uintptr_t)p->mem + p->size + 0xfff) & ~0xfffull;
                if (!nvn_gpu_only((uintptr_t)p->mem) && p->size) cap_mem_commit(lo, hi - lo);
                if (!b->pool_init((pool_t *)(uintptr_t)a)) rt_fatal("cap: pool_init of %#llx refused", (unsigned long long)a);
            }
            if (h.type == CAP_POOL_FINI) b->pool_fini((pool_t *)(uintptr_t)a);
            if (h.type == CAP_PROGREL) b->program_release((const prog_t *)(uintptr_t)a);
            break;
        }
        case CAP_MAPV: {
            uint64_t a[2];
            cap_in_read(in, a, sizeof a);
            cap_in_read(in, buf, a[1] * 0x28);
            if (!b->pool_map_virtual((pool_t *)(uintptr_t)a[0], (int)a[1], buf)) rt_fatal("cap: MapVirtual refused");
            break;
        }
        case CAP_TEXREG: {
            CapTexReg r;
            cap_in_read(in, &r, sizeof r);
            b->texture_register(r.id, &r.t, r.has_view ? &r.v : NULL);
            break;
        }
        case CAP_SAMPREG: {
            int32_t id;
            samp_par s;
            cap_in_read(in, &id, sizeof id);
            cap_in_read(in, &s, sizeof s);
            b->sampler_register(id, &s);
            break;
        }
        case CAP_TEXREL: {
            tex_t t;
            cap_in_read(in, &t, sizeof t);
            b->texture_release(&t);
            break;
        }
        case CAP_SUBMIT: {
            if (!seen_submit++) t_state = plat_now_ns() - t_start;
            int64_t n;
            cap_in_read(in, &n, sizeof n);
            uint64_t *hs = calloc((size_t)n + 1, sizeof *hs);
            if (!hs) rt_fatal("cap: out of memory");
            // MKW_CAP_SUBLIST=1 (a measurement): each submission's recordings with their size and a hash of their bytes
            // before the texture pointers are patched in, so the same recording handed again in a later frame shows.
            static int sublist = -1;
            if (sublist < 0) sublist = cfg_get("MKW_CAP_SUBLIST") && atoi(cfg_get("MKW_CAP_SUBLIST"));
            char line[2048];
            int lo = sublist ? snprintf(line, sizeof line, "cap: submit %llu frame %llu: %lld recordings:",
                                        (unsigned long long)submits, (unsigned long long)frame, (long long)n) : 0;
            for (int64_t i = 0; i < n; i++) {
                uint64_t hd[2];
                cap_in_read(in, hd, sizeof hd);
                uint8_t *rec = plat_aligned_alloc((hd[0] + 15) & ~15ull, 16);   // kept: payloads live as long as the run
                uint32_t *offs = malloc(4 * hd[1] + 4);
                if (!rec || !offs) rt_fatal("cap: out of memory for a recording");
                cap_in_read(in, rec, hd[0]);
                cap_in_read(in, offs, 4 * hd[1]);
                // MKW_CAP_RECS=<dir> (a measurement): recordings over 64 KiB saved there as they are in the capture, to
                // compare one frame's ops with the next (tools/analisis/recdiff.py).
                const char *rd = cfg_get("MKW_CAP_RECS");
                if (rd && hd[0] > (64u << 10)) {
                    char rp[1024];
                    snprintf(rp, sizeof rp, "%s/rec-%llu-%lld.bin", rd, (unsigned long long)submits, (long long)i);
                    FILE *f = fopen(rp, "wb");
                    if (f) fwrite(rec, 1, hd[0], f), fclose(f);
                }
                if (sublist && lo < (int)sizeof line - 48) {
                    uint64_t fnv = 0xcbf29ce484222325ull;
                    for (uint64_t k = 0; k < hd[0]; k++) fnv = (fnv ^ rec[k]) * 0x100000001b3ull;
                    lo += snprintf(line + lo, sizeof line - (size_t)lo, " %llu B %016llx", (unsigned long long)hd[0],
                                   (unsigned long long)fnv);
                }
                for (uint64_t k = 0; k < hd[1]; k++) {
                    tex_t t;
                    cap_in_read(in, &t, sizeof t);
                    const tex_t *p = pin(&t);
                    memcpy(rec + offs[k], &p, sizeof p);
                }
                free(offs);
                hs[i] = (uint64_t)(uintptr_t)rec;
            }
            if (sublist) rt_log("%s", line);
            uint64_t t0 = plat_now_ns(), c0 = plat_self_cpu_ns();
            b->submit(hs, (int)n);
            sub_ns += plat_now_ns() - t0, sub_cpu += plat_self_cpu_ns() - c0;
            submits++;
            free(hs);
            break;
        }
        case CAP_PRESENT: {
            CapPresent p;
            cap_in_read(in, &p, sizeof p);
            // The frame's submissions replayed, which a recording change shows in frames past the first two (those build
            // the pipelines the draws need, MKW_PIPE_SYNC).
            // MKW_TRACE=perf: the GPU's busy time for the frame too (its timestamps), after waiting for it: what a
            // recording change costs the GPU, measured without the game (tlab2, 07/11/2026: chunk cuts +5 ms a frame).
            char gs[96] = "";
            if ((rt_trace & RT_TRACE_PERF) && perf_gpu_stats) {
                b->finish();
                PerfGpu g = {0};
                perf_gpu_stats(&g);
                if (g.have_gpu)
                    snprintf(gs, sizeof gs, ", GPU busy %.2f ms (%llu parts timed, %llu not)", g.gpu_ns / 1e6,
                             (unsigned long long)g.gpu_parts, (unsigned long long)g.gpu_missed);
            }
            rt_log("cap: frame %llu: submits %.1f ms (CPU %.1f ms)%s", (unsigned long long)(m.first_frame + presents),
                   (double)(sub_ns - frame_sub0) / 1e6, (double)(sub_cpu - frame_cpu0) / 1e6, gs);
            frame_cpu0 = sub_cpu;
            // And what holds the GPU memory, with the objects waiting for their submission: the chunked replay peaked at
            // ~12 GB against the serial one (ht1: destroyed objects tied to the value being recorded).
            if (rt_trace & RT_TRACE_PERF) {
                vram_log_now();
                uint64_t rd, sk, ah, bl;
                vk_exec_dc_stats(&rd, &sk, &ah, &bl);
                rt_log("cap: frame %llu: layered depth clears: %llu of every layer, %llu skipped, %llu layer by layer in a "
                       "chunk", (unsigned long long)(m.first_frame + presents), (unsigned long long)ah,
                       (unsigned long long)sk, (unsigned long long)bl);
            }
            frame_sub0 = sub_ns;
            // MKW_TRACE=draw: prepare_draw's stages per draw for the frame (as MKW_TRACE=perf,draw prints them in a run),
            // the TSC's rate taken from this frame's span: a recording change measured without the game.
            if (rt_trace & RT_TRACE_DRAW) {
                static uint64_t tsc0, ns0;
                uint64_t tsc = plat_cycles(), ns = plat_now_ns();
                char tb[900];
                if (tsc0 && vk_exec_draw_timing(tb, sizeof tb, (double)(ns - ns0) / 1e3 / (double)(tsc - tsc0)))
                    rt_log("cap: frame %llu: prepare_draw per draw, us:%s", (unsigned long long)(m.first_frame + presents), tb);
                tsc0 = tsc, ns0 = ns;
            }
            vk_exec_op_timing_log();    // MKW_TRACE=draw: per frame
            vk_exec_draw_probe_log();   // MKW_DRAW_CACHE_PROBE: per frame
            vk_exec_draw_cache_log();   // MKW_DRAW_CACHE: per frame
            ru_log();                   // MKW_REUSE: per frame
            rec_stats_log();            // MKW_TRACE=perf: the recorder ring's waits, per frame
            const tex_t *t = pin(&p.t);
            b->present(t, p.has_crop ? p.crop : NULL, 0);
            presents++;
            char png[1200];
            // The first frame is saved apart (warm-*), not compared: its foliage reads zero in about half the replays
            // (cause open); the frames after it are the same in every replay.
            snprintf(png, sizeof png, "%s/%s-f%llu.png", outdir, presents == 1 ? "warm" : tag,
                     (unsigned long long)(m.first_frame + presents - 1));
            save_png(b, t, png);
            rt_log("cap: present %llu -> %s", (unsigned long long)(m.first_frame + presents - 1), png);
            break;
        }
        case CAP_WTEX: {
            CapWtex w;
            cap_in_read(in, &w, sizeof w);
            cap_in_read(in, buf, w.bytes);
            b->write_texels(&w.t, w.has_view ? &w.v : NULL, &w.r, buf, w.row, w.img);
            break;
        }
        case CAP_WIMG: {
            CapWimg w;
            cap_in_read(in, &w, sizeof w);
            cap_in_read(in, buf, w.bytes);
            // The image here is made by this build's rules; with another ASTC setting or GPU than the capture's it has
            // another format and stays as the back end leaves it (the texture then replays as its storage decodes).
            if (!vk_texture_decoded_copy(&w.t, (VkFormat)w.format, w.level, w.layer, w.nlayers, buf, w.bytes, 1) &&
                !wimg_skipped++)
                rt_log("cap: a decoded ASTC image of the capture (%ux%u, VkFormat %u) has another format or size "
                       "here (another MKW_ASTC_* setting or GPU): left as its storage decodes", w.t.d.width,
                       w.t.d.height, w.format);
            break;
        }
        case CAP_GWRITTEN: {
            uint64_t a[2];
            cap_in_read(in, a, sizeof a);
            b->guest_written(a[0], a[1]);
            break;
        }
        case CAP_GPUMEM: {
            uint64_t a[2];
            cap_in_read(in, a, sizeof a);
            cap_in_read(in, buf, a[1]);
            if (!vk_mem_phys_load(a[0], buf, a[1]))
                rt_log("cap: physical pool chunk %#llx+%#llx has no pool here", (unsigned long long)a[0],
                       (unsigned long long)a[1]);
            break;
        }
        case CAP_SPARSE: {
            uint64_t a[2];
            uint8_t key[64];
            cap_in_read(in, a, sizeof a);
            if (a[0] > sizeof key) rt_fatal("cap: sparse slot key of %llu bytes", (unsigned long long)a[0]);
            cap_in_read(in, key, a[0]);
            cap_in_read(in, buf, a[1]);
            vk_sparse_load(key, (uint32_t)a[0], buf, a[1]);
            break;
        }
        case CAP_FINISH: b->finish(); break;
        case CAP_FRAME: cap_in_read(in, &frame, sizeof frame); break;
        default: rt_fatal("cap: unknown record %u", h.type);
        }
    }
    cap_in_close(in);
    b->finish();
    vk_pt_flush(frame, presents);   // MKW_PASS_TIMING: the GPU time of the replayed frames
    vk_mem_replay_log(presents);    // mirror and shadow traffic per presented frame (MKW_MEMORY=mirror)
    long errors = vk_validation_errors_total();
    vk_exec_par_log();   // how many submissions went in chunks: a replay that never chunks compares serial with serial
    static const char *const names[] = {"", "meta", "mem", "zero", "pool init", "pool fini", "mapv", "texreg", "sampreg",
                                        "texrel", "progrel", "submit", "present", "wtex", "gwritten", "finish", "frame",
                                        "end", "gpumem", "sparse", "wimg"};
    char times[512] = "";
    for (int t = 1, o = 0; t <= CAP_LAST && o < (int)sizeof times - 40; t++)
        if (type_ns[t] >= 100000000)   // 0.1 s or more
            o += snprintf(times + o, sizeof times - (size_t)o, " %s %.1f s (%llu)", names[t], (double)type_ns[t] / 1e9,
                          (unsigned long long)type_n[t]);
    printf("replay: %s: time by record:%s\n", tag, times);
    vk_usnap_summary();   // the uniform snapshot's per-submission costs, which a run logs every 3600 submissions
    {   // MKW_LOCK_PROF=1: the lock waits of the whole replay (a run logs them every 1200 submissions, vk_sync.c)
        char lb[1024];
        if (plat_lock_report(0, lb, sizeof lb)) printf("replay: %s: lock waits (MKW_LOCK_PROF):%s\n", tag, lb);
    }
    printf("replay: %s: state %.1f s (%.0f MiB of memory), %llu submits replayed in %.1f ms, %llu presents, "
           "%ld validation errors, total %.1f s\n",
           tag, (double)t_state / 1e9, (double)mem_bytes / (1 << 20), (unsigned long long)submits, (double)sub_ns / 1e6,
           (unsigned long long)presents, errors, (double)(plat_now_ns() - t_start) / 1e9);
    fflush(stdout);
    plat_exit(errors ? 1 : 0);
}
