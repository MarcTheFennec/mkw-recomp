// MKW_DUMP=<frame>: a frame dump for the Vulkan back end, which RenderDoc cannot capture (it hides
// VK_EXT_external_memory_host). Between presents frame-1 and frame, the replay's command buffers also copy, after each
// render pass, its colour and depth targets and, after each dispatch, the storage images it writes into host buffers;
// the completion thread turns them into 8-bit PNGs once the submission is done. 3D textures (LUTs, volumes) sampled or
// written in the frame are copied once each, slices side by side. Files in MKW_DUMP_DIR (default "dump"):
//   pass_<seq>_t<i>_<w>x<h>_<vkformat>_<name>.png   colour target i (td: depth; img<slot>: storage image; lut<id>: 3D;
//                                                    a BC target drawn through a block view: its blocks as that view)
//   ..._tm.png                                       float formats: x/(1+x) of the same image (the plain one clamps)
//   passes.txt   per pass or dispatch: targets, draws, each distinct pipeline + texture set with its shaders' SPIR-V
//                hashes (sh_<hash>.spv written once) and the bound textures with their samplers
//   images.txt   per image: channel min/max, distinct texel values over the image and over its top quarter
//   stats.txt    per render pass: pipeline statistics (input primitives, VS invocations, clipping in/out, FS invocations)
// Sampled 8-bit 2D textures up to TEX2D_CAP texels are copied once each as pass_<seq>_tex<id>_... at the end of the pass
// that samples them; texture lines carry the storage address, mirror state, upload count and the image's and slot's
// swizzle. Copies and texture clears are logged as "copy/clear after pass N". MKW_DUMP_LOG=<from> also logs the
// frames from..frame-1 to passes.txt, with PNGs only of targets up to 64K texels. MKW_DUMP_BUF=<addr>:<size>[:...]
// snapshots one GPU buffer range after every logged pass (buf_<pass>.bin and bufs.txt, see buf_snap).
// PNGs are opaque (alpha dropped). Integer formats are scaled by the image maximum, depth by its min..max range, SNORM
// as x/2+1/2, UNORM and sRGB bytes written as stored. Caps: MAX_PASSES passes copied, PNG_CAP bytes of PNGs.
// MKW_DUMP_TEX=<id>|<w>x<h>[,...]: those texture slots, or 2D textures of that size, are copied where first sampled,
// whatever their format.
// MKW_DUMP_UBO=1: each listed pipeline also prints the first 32 vec4 (MKW_DUMP_UBO_ROWS=from-to adds those rows, and
// rows holding a NaN or an infinity are listed) of its user uniform blocks (c3 and up), read from
// guest memory when the entry is listed. A block the submission rewrites with UpdateUniformBuffer shows its value from
// before the submission (vk_draw_internal.h upd_note).
// MKW_DUMP_SKIP_RUNS=1: a render pass with the same targets as the render pass before it is logged but not copied
// (the terrain decals draw one pass each into the G-buffer and would use up both caps).
// MKW_DUMP_DRAWS: a copy of the first colour target after each draw of a run of passes (vk_dump_draws.c).
// The parts: vk_dump_img.c (formats, image copies and PNGs), vk_dump_buf.c (MKW_DUMP_BUF) and vk_dump_draws.c
// (MKW_DUMP_DRAWS), sharing vk_dump_internal.h.
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "nvn_api.h"
#include "vk.h"
#include "vk_exec.h"
#include "platform/platform.h"
#include "vk_dump_internal.h"


// The host buffers come from an allocator of their own: the runtime's has MKW_GPU_MEM_LIMIT's heap limits, which a
// course already fills (run101-run102: every large target "host buffer allocation failed"), and a run without the
// limit took the GPU's memory from the desktop too (run103: kwin's submissions failed). INFLIGHT_CAP bounds this one.
static VmaAllocator dvma_;
static PlatOnce dvma_once = PLAT_ONCE_INIT;

static void dvma_init(void *arg) {
    (void)arg;
    const VkCtx *c = vk_ctx();
    VmaAllocatorCreateInfo ai = {.physicalDevice = c->phys, .device = c->device, .instance = c->instance,
                                 .vulkanApiVersion = VK_API_VERSION_1_3};
    if (vmaCreateAllocator(&ai, &dvma_) != VK_SUCCESS) rt_fatal("vk_dump: vmaCreateAllocator failed");
}

VmaAllocator dvma(void) {
    plat_once(&dvma_once, dvma_init, NULL);
    return dvma_;
}

// Completion callbacks and rtstats workers no longer hold readback buffers.
void vk_dump_shutdown(void) {
    if (dvma_) vmaDestroyAllocator(dvma_);
    dvma_ = NULL;
    dvma_once = (PlatOnce)PLAT_ONCE_INIT;
}
#define MAX_ENTS 48                 // distinct pipeline + texture sets listed per pass
#define MAX_LUTS 256                // 3D textures and small sampled 8-bit 2D textures dumped per frame
#define TEX2D_CAP (1u << 20)        // texels of a sampled 2D texture that is dumped

int vk_dump_armed;
uint64_t dump_frame;
static uint64_t log_from;   // MKW_DUMP_LOG: first frame logged to passes.txt without images
char dump_dir[400];
FILE *images_f;
FILE *passes_f;
static int seq, finished, copied;      // copied: passes and dispatches of the dump frame whose images were copied
static int skip_runs, dump_ubo;        // MKW_DUMP_SKIP_RUNS, MKW_DUMP_UBO
static int luts_only;                  // MKW_CTL "dump DIR lut": only the 3D volumes and small textures, no targets
static uint32_t want_tex[16][2];       // MKW_DUMP_TEX: slot id and 0, or width and height
static int nwant_tex;
static uint64_t last_targets;          // targets_sig of the last render pass; 0 after a dispatch
static const char *pass_skip;          // why the current pass copies nothing, or NULL
volatile int64_t png_bytes, inflight;
PlatMutex img_lock = PLAT_MUTEX_INIT;
PlatMutex rec_lock = PLAT_MUTEX_INIT;

static int active(void) {
    uint64_t f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + 1;
    return !finished && (f == dump_frame || (log_from && f >= log_from && f < dump_frame));
}
uint64_t frame_now(void) { return __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + 1; }

typedef struct { char *s; size_t n, cap; } Str;

__attribute__((format(printf, 2, 3))) static void sp(Str *b, const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    int k = vsnprintf(NULL, 0, fmt, a);
    va_end(a);
    if (k < 0) return;
    if (b->n + (size_t)k + 1 > b->cap) {
        b->cap = (b->n + (size_t)k + 1) * 2;
        b->s = realloc(b->s, b->cap);
        if (!b->s) rt_fatal("vk_dump: out of memory");
    }
    va_start(a, fmt);
    vsnprintf(b->s + b->n, (size_t)k + 1, fmt, a);
    va_end(a);
    b->n += (size_t)k;
}

// SPIR-V hash of a shader. The file is written the first time a logged pass names the shader, not on a hash looked up
// only to decide whether the pass is the one MKW_DUMP_DRAWS asked for.
uint64_t shader_hash(const GfxShader *s) {
    static struct { const GfxShader *s; uint64_t h; int wrote; } tab[1024];
    static int n;
    if (!s) return 0;
    int i = 0;
    while (i < n && tab[i].s != s) i++;
    if (i == n) {
        uint64_t words = 0, h = 0xcbf29ce484222325ull;
        const uint32_t *w = gfx_shader_spirv(s, &words);
        const uint8_t *b = (const uint8_t *)w;
        for (uint64_t k = 0; k < words * 4; k++) h = (h ^ b[k]) * 0x100000001b3ull;
        if (n == 1024) {
            if (passes_f) {
                char path[600];
                snprintf(path, sizeof path, "%s/sh_%016llx.spv", dump_dir, (unsigned long long)h);
                FILE *f = fopen(path, "wb");
                if (f) fwrite(w, 4, (size_t)words, f), fclose(f);
            }
            return h;
        }
        tab[n].s = s, tab[n].h = h, tab[n].wrote = 0;
        i = n++;
    }
    if (passes_f && !tab[i].wrote) {
        char path[600];
        uint64_t words = 0;
        const uint32_t *w = gfx_shader_spirv(s, &words);
        snprintf(path, sizeof path, "%s/sh_%016llx.spv", dump_dir, (unsigned long long)tab[i].h);
        FILE *f = fopen(path, "wb");
        if (f) fwrite(w, 4, (size_t)words, f), fclose(f);
        tab[i].wrote = 1;
    }
    return tab[i].h;
}

// Per pass: distinct pipeline + texture sets, draws, 3D textures to copy at its end.
typedef struct { VkPipeline p; uint64_t th; int draws; char *text; } Ent;
static Ent ents[MAX_ENTS];
static int nents, ents_over, pass_draws;
static struct { uint64_t fs; int draws; } over_fs[64];   // fragment shaders of the draws past MAX_ENTS
static int n_over_fs;
typedef struct { VkImage image; VkFormat format; uint32_t w, h, d; int id, is2d; } Lut;
static Lut luts[MAX_LUTS];          // queued for the next pass end or dispatch
static int nluts;
static VkImage luts_done[MAX_LUTS];
static int nluts_done;

// wanted: MKW_DUMP_TEX asked for it. The small sampled textures filled the MAX_LUTS slots before the HUD's glyph
// cache came up in its last passes (vis13, 01/11/2026): the last 16 slots are kept for wanted textures.
static void queue_lut(const VkTexImage *t, int id, int wanted) {
    if (frame_now() != dump_frame) return;
    for (int i = 0; i < nluts_done; i++)
        if (luts_done[i] == t->image) return;
    if (nluts_done >= MAX_LUTS - (wanted ? 0 : 16)) return;
    luts_done[nluts_done++] = t->image;
    luts[nluts++] = (Lut){t->image, t->format, t->width, t->height, t->type == VK_IMAGE_TYPE_3D ? t->depth : 1, id,
                          t->type != VK_IMAGE_TYPE_3D};
}

// The uncompressed format of a BC format's block size, UNDEFINED for other formats: a BC texture is dumped as its raw
// blocks (one texel per block), which shows at least whether it was ever written.
static VkFormat block_format(VkFormat f) {
    if (f == VK_FORMAT_BC4_UNORM_BLOCK || f == VK_FORMAT_BC4_SNORM_BLOCK ||
        (f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_BC1_RGBA_SRGB_BLOCK))
        return VK_FORMAT_R32G32_UINT;
    if (f >= VK_FORMAT_BC2_UNORM_BLOCK && f <= VK_FORMAT_BC7_SRGB_BLOCK) return VK_FORMAT_R32G32B32A32_UINT;
    return VK_FORMAT_UNDEFINED;
}

static void flush_luts(VkExec *ex, int n) {
    char fn[16];
    for (int i = 0; i < nluts; i++) {
        const Lut *l = &luts[i];
        char name[160];
        VkFormat bf = block_format(l->format);
        snprintf(name, sizeof name, "pass_%04d_%s%d_%ux%ux%u_%d_%s%s", n, l->is2d ? "tex" : "lut", l->id, l->w, l->h, l->d,
                 (int)l->format, fmt_name(l->format, fn), bf ? "_blocks" : "");
        const char *why = bf ? copy_region(ex, l->image, bf, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, (VkOffset3D){0, 0, 0},
                                           (VkExtent3D){l->w, l->h, l->d}, (l->w + 3) / 4, (l->h + 3) / 4, l->d, name)
                             : copy_out(ex, l->image, l->format, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, l->w, l->h, l->d, name);
        fprintf(passes_f, "  %s tex %d -> %s.png%s%s\n", l->is2d ? "2D" : "3D", l->id, name, why ? " skipped: " : "",
                why ? why : "");
    }
    nluts = 0;
}

static int nvn_stage(int gfx_stage) {
    if (gfx_stage & VK_PIPE_STAGE_NVN) return gfx_stage & 7;   // task or mesh binding
    return gfx_stage == GFX_STAGE_VS ? 0 : gfx_stage == GFX_STAGE_FS ? 1 : 5;
}

static const char *decl_type(uint16_t t, char tmp[24]) {
    static const char *base[] = {"?", "1D", "buffer", "2D", "3D", "cube"};
    snprintf(tmp, 24, "%s%s%s%s", base[(t & 0xff) < 6 ? t & 0xff : 0], t & 0x100 ? "Array" : "", t & 0x200 ? "MS" : "",
             t & 0x400 ? "Shadow" : "");
    return tmp;
}

static const char *wrap_name(int w) {
    static const char *n[] = {"clamp", "repeat", "mirror_clamp", "mirror_clamp_edge", "mirror_clamp_border",
                              "clamp_border", "mirrored_repeat", "clamp_edge"};
    return w >= 0 && w < 8 ? n[w] : "?";
}

static uint64_t handle_of(const VkExec *ex, int st, int slot, int img) {
    if (img) return slot >= 0 && slot < 32 ? ex->img[st][slot] : 0;
    return slot >= 0 && slot < 128 ? ex->tex[st][slot] : 0;
}

// The textures, images and samplers of info's bindings; 3D textures are queued for a copy.
static void describe_bindings(Str *b, const VkExec *ex, const VkPipeInfo *info) {
    static const char *stn[8] = {"vs", "fs", "?", "?", "?", "cs", "task", "mesh"};
    for (uint32_t i = 0; i < info->n_bindings; i++) {
        const VkPipeBinding *bd = &info->bindings[i];
        const GfxShaderRes *r = bd->res;
        if (r && bd->kind == GFX_RES_SSBO) {   // where the address lives (c0: the stage's NVN slot table) and the stage's slots
            int st = nvn_stage(bd->stage);
            sp(b, "    %s ssbo at c%d+%#x%s, bound:", stn[st], r->cbuf_slot, r->handle * 4, r->flags & 1 ? " written" : "");
            for (int k = 0; k < 16; k++)
                if (ex->ssbo[st][k].addr)
                    sp(b, " %d=%#llx+%#llx", k, (unsigned long long)ex->ssbo[st][k].addr, (unsigned long long)ex->ssbo[st][k].size);
            sp(b, "\n");
            continue;
        }
        if (r && bd->kind == GFX_RES_UBO && dump_ubo && r->slot >= 3 && r->slot - 3 < 18) {
            int st = nvn_stage(bd->stage);
            const p_bind_buf *u = &ex->ubo[st][r->slot - 3];
            VkMemSpan ms;
            sp(b, "    %s ubo c%d addr %#llx size %#llx", stn[st], r->slot, (unsigned long long)u->addr, (unsigned long long)u->size);
            if (u->addr && vk_mem_resolve(u->addr, 1, &ms)) {
                uint64_t n = u->size && u->size < ms.avail ? u->size : ms.avail;
                const float *f = (const float *)(uintptr_t)u->addr;
                for (uint64_t k = 0; k < 32 && (k + 1) * 16 <= n; k++)
                    sp(b, "%s[%llu] %g %g %g %g", k % 4 ? " " : "\n      ", (unsigned long long)k, f[4 * k], f[4 * k + 1],
                       f[4 * k + 2], f[4 * k + 3]);
                const char *rows = cfg_get("MKW_DUMP_UBO_ROWS");   // "from-to": those rows too
                unsigned r0 = 0, r1 = 0;
                if (rows && sscanf(rows, "%u-%u", &r0, &r1) == 2)
                    for (uint64_t k = r0; k <= r1 && (k + 1) * 16 <= n; k++)
                        sp(b, "%s[%llu] %g %g %g %g", (k - r0) % 4 ? " " : "\n      rows: ", (unsigned long long)k, f[4 * k],
                           f[4 * k + 1], f[4 * k + 2], f[4 * k + 3]);
                int shown = 0;   // rows past the first 32 holding a NaN or an infinity, with their values
                for (uint64_t k = 32; (k + 1) * 16 <= n && shown < 16; k++)
                    if (!isfinite(f[4 * k]) || !isfinite(f[4 * k + 1]) || !isfinite(f[4 * k + 2]) || !isfinite(f[4 * k + 3]))
                        sp(b, "%s[%llu] %g %g %g %g", shown++ % 4 ? " " : "\n      not finite: ", (unsigned long long)k,
                           f[4 * k], f[4 * k + 1], f[4 * k + 2], f[4 * k + 3]);
            } else {
                sp(b, " (not mapped)");
            }
            sp(b, "\n");
            continue;
        }
        if (!r || (bd->kind != GFX_RES_TEX && bd->kind != GFX_RES_IMG)) continue;
        int st = nvn_stage(bd->stage), img = bd->kind == GFX_RES_IMG;
        char dt[24], fn[16];
        if (bd->type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || bd->type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
            sp(b, "    %s %s slot %d: buffer texture (zero view bound)\n", stn[st], img ? "img" : "tex", r->slot);
            continue;
        }
        for (uint32_t k = 0; k < (bd->count ? bd->count : 1u); k++) {
            int slot = r->slot + (int)k;
            uint64_t h = handle_of(ex, st, slot, img);
            sp(b, "    %s %s slot %d decl %s%s: ", stn[st], img ? "img" : "tex", slot, decl_type(r->type, dt),
               img && (r->flags & 4) ? " store" : "");
            if (!h) { sp(b, "no handle (dummy)\n"); continue; }
            view_t v;
            int hv = 0;
            const VkTexImage *t = vk_texture_by_id(NVN_HANDLE_TEX(h), &v, &hv);
            if (!t) { sp(b, "tex %d unresolved (dummy)\n", NVN_HANDLE_TEX(h)); continue; }
            static const char *vt[] = {"1D", "2D", "3D", "cube", "1DArray", "2DArray", "cubeArray"};
            sp(b, "tex %d %s(%d) %ux%ux%u levels %u layers %u image %s", NVN_HANDLE_TEX(h), fmt_name(t->format, fn),
               (int)t->format, t->width, t->height, t->depth, t->levels, t->layers,
               t->view_type <= 6 ? vt[t->view_type] : "?");
            sp(b, " slot-nvn-fmt %#x", vk_texture_slot_format(NVN_HANDLE_TEX(h), NULL));
            if (hv && v.set_format) sp(b, " view-nvn-fmt %#x", v.format);
            if (hv && v.set_levels) sp(b, " view-levels %u+%u", v.base_level, v.nlevels);
            if (hv && v.set_layers) sp(b, " view-layers %u+%u", v.min_layer, v.nlayers);
            samp_par s;
            if (img) {
                // storage image: no sampler
            } else if (r->separate) {
                sp(b, " | separate sampler (not replayed: default sampler)");
            } else if (vk_sampler_params(NVN_HANDLE_SAMP(h), &s)) {
                static const char *minn[] = {"N", "L", "N_mipN", "L_mipN", "N_mipL", "L_mipL"};
                int mf = s.minf;
                sp(b, " | samp %d: mag %s min %s (vk min %s mip %s) wrap %s/%s/%s aniso %g lod %g..%g bias %g%s",
                   NVN_HANDLE_SAMP(h), s.magf == 1 ? "L" : "N", mf < 6 ? minn[mf] : "?",
                   mf == 1 || mf == 3 || mf == 5 ? "L" : "N", mf <= 1 ? "none" : mf >= 4 ? "L" : "N",
                   wrap_name(s.ws), wrap_name(s.wt), wrap_name(s.wr), s.aniso, s.lod_min, s.lod_max, s.lod_bias,
                   s.cmp_mode ? " compare" : "");
            } else {
                sp(b, " | samp %d empty (default sampler)", NVN_HANDLE_SAMP(h));
            }
            uint32_t ups;
            int last, wr;
            vk_texture_debug(t, &ups, &last, &wr);
            sp(b, " | addr %#llx image %p mirrored %d gpu_owned %d bound %d uploads %u last-check %d written-since %d",
               (unsigned long long)t->addr, (void *)t->image, t->mirrored, t->gpu_owned, t->bound, ups, last, wr);
            uint8_t ssw[4] = {0};
            vk_texture_slot_swz(NVN_HANDLE_TEX(h), ssw);
            sp(b, " | swz image %u%u%u%u slot %u%u%u%u", t->swz[0], t->swz[1], t->swz[2], t->swz[3], ssw[0], ssw[1], ssw[2],
               ssw[3]);
            if (hv && v.set_swizzle) sp(b, " view %u%u%u%u", v.swz[0], v.swz[1], v.swz[2], v.swz[3]);
            sp(b, "\n");
            int wanted = 0;
            for (int k = 0; k < nwant_tex; k++)
                wanted |= want_tex[k][1] ? want_tex[k][0] == t->width && want_tex[k][1] == t->height
                                         : want_tex[k][0] == (uint32_t)NVN_HANDLE_TEX(h);
            if (t->type == VK_IMAGE_TYPE_3D) queue_lut(t, NVN_HANDLE_TEX(h), 0);
            else if (wanted && t->type == VK_IMAGE_TYPE_2D && t->samples == 1) queue_lut(t, NVN_HANDLE_TEX(h), 1);
            else if (!img && t->type == VK_IMAGE_TYPE_2D && t->samples == 1 && (uint64_t)t->width * t->height <= TEX2D_CAP &&
                     (t->format == VK_FORMAT_R8_UNORM || t->format == VK_FORMAT_R8G8_UNORM ||
                      t->format == VK_FORMAT_R8G8B8A8_UNORM || t->format == VK_FORMAT_R8G8B8A8_SRGB ||
                      t->format == VK_FORMAT_B8G8R8A8_UNORM || t->format == VK_FORMAT_B8G8R8A8_SRGB))
                queue_lut(t, NVN_HANDLE_TEX(h), 0);
        }
    }
}

static uint64_t bindings_hash(const VkExec *ex, const VkPipeInfo *info) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t i = 0; i < info->n_bindings; i++) {
        const VkPipeBinding *bd = &info->bindings[i];
        if (bd->res && bd->kind == GFX_RES_SSBO) {
            int st = nvn_stage(bd->stage), slot = bd->res->slot;
            if (slot >= 0 && slot < 16) h = (h ^ ex->ssbo[st][slot].addr) * 0x100000001b3ull;
            continue;
        }
        if (!bd->res || (bd->kind != GFX_RES_TEX && bd->kind != GFX_RES_IMG)) continue;
        for (uint32_t k = 0; k < (bd->count ? bd->count : 1u); k++)
            h = (h ^ handle_of(ex, nvn_stage(bd->stage), bd->res->slot + (int)k, bd->kind == GFX_RES_IMG)) * 0x100000001b3ull;
    }
    return h;
}

// MKW_TRACE_UBO=<fs hash>[:<rows>] (diagnosis): every draw with that fragment shader appends its user uniform blocks
// (c3 and up, the first <rows> vec4, 32 by default), read at record time, to MKW_TRACE_UBO_OUT (ubo_trace.txt) as
// "pass <frame>-<n>:" blocks in passes.txt's format, so tools/shaders/glsl_cpu.py runs the shader on them. Catches a
// pass whose frame changes from run to run (the colour-grading LUT's last write fell anywhere in frames 230-252).
static uint64_t tu_fs;
static unsigned tu_rows = 32, tu_n;
static FILE *tu_f;

static void trace_ubo(VkExec *ex, const VkPipeState *s) {
    if (!tu_fs || !tu_f || shader_hash(s->fs) != tu_fs) return;
    int st = nvn_stage(GFX_STAGE_FS);
    fprintf(tu_f, "pass %llu-%u: draw, fence %llu\n", (unsigned long long)frame_now(), tu_n++, (unsigned long long)ex->fence);
    for (int k = 0; k < 18; k++) {
        const p_bind_buf *u = &ex->ubo[st][k];
        VkMemSpan ms;
        if (!u->addr || !vk_mem_resolve(u->addr, 1, &ms)) continue;
        uint64_t n = u->size && u->size < ms.avail ? u->size : ms.avail;
        const float *f = (const float *)(uintptr_t)u->addr;
        fprintf(tu_f, "    fs ubo c%d addr %#llx size %#llx", k + 3, (unsigned long long)u->addr, (unsigned long long)u->size);
        for (uint64_t r = 0; r < tu_rows && (r + 1) * 16 <= n; r++)
            fprintf(tu_f, "%s[%llu] %.9g %.9g %.9g %.9g", r % 4 ? " " : "\n      ", (unsigned long long)r, f[4 * r],
                    f[4 * r + 1], f[4 * r + 2], f[4 * r + 3]);
        fprintf(tu_f, "\n");
    }
    fflush(tu_f);
}

void vk_dump_draw(VkExec *ex, const VkPipeState *s, VkPipeline p, const VkPipeInfo *info) {
    plat_mutex_lock(&rec_lock);
    trace_ubo(ex, s);
    if (!active()) goto out;
    draws_select(ex, s, seq);
    pass_draws++;
    uint64_t th = bindings_hash(ex, info);
    for (int i = 0; i < nents; i++)
        if (ents[i].p == p && ents[i].th == th) {
            ents[i].draws++;
            goto out;
        }
    if (nents == MAX_ENTS) {
        ents_over++;
        uint64_t fh = shader_hash(s->fs);
        int k = 0;
        while (k < n_over_fs && over_fs[k].fs != fh) k++;
        if (k < 64) {
            if (k == n_over_fs) over_fs[n_over_fs++].fs = fh, over_fs[k].draws = 0;
            over_fs[k].draws++;
        }
        goto out;
    }
    Str b = {0};
    sp(&b, "vs %016llx fs %016llx prog %llu prim %d; depth test %d write %d func %d; wmask t0 %#x t1 %#x; blend0 %llx "
           "fs outputs %#x; discard %d vp %.0f,%.0f %.0fx%.0f z %.2f-%.2f range %.2f-%.2f scissor %d,%d %dx%d (set %d) samples %u\n",
       (unsigned long long)shader_hash(s->vs),
       (unsigned long long)shader_hash(s->fs), (unsigned long long)(ex->prog ? ex->prog->serial : 0), s->prim,
       (int)s->ds.dt, (int)s->ds.dw, (int)s->ds.df, vk_pipeline_target_wmask(s, 0), vk_pipeline_target_wmask(s, 1),
       (unsigned long long)*(const uint64_t *)&s->blend[0], s->fs ? gfx_shader_outputs(s->fs, NULL) : 0,
       (int)s->raster_discard, ex->vp[0][0], ex->vp[0][1], ex->vp[0][2], ex->vp[0][3], ex->vp[0][4], ex->vp[0][5],
       ex->depth_range[0], ex->depth_range[1], ex->scissor.x, ex->scissor.y, ex->scissor.w, ex->scissor.h,
       ex->has_scissor, ex->pass_samples);
    for (int k = 0; k < 16; k++) {   // the vertex input: attributes with a format, their streams and buffers
        uint32_t w = ex->pipe.vattr[k];
        va_st a;
        memcpy(&a, &w, sizeof a);
        if (!a.fmt) continue;
        const NVNbufferRange *vb = a.stream < 16 ? &ex->vbuf[a.stream] : NULL;
        sp(&b, "    vattr %d: fmt %#x offset %u stream %u stride %u divisor %u buffer %#llx+%#llx\n", k, a.fmt, a.offset,
           a.stream, a.stream < 16 ? ex->pipe.vstream[a.stream].stride : 0, a.stream < 16 ? ex->pipe.vstream[a.stream].divisor : 0,
           vb ? (unsigned long long)vb->address : 0ull, vb ? (unsigned long long)vb->size : 0ull);
    }
    describe_bindings(&b, ex, info);
    ents[nents++] = (Ent){p, th, 1, b.s};
out:
    plat_mutex_unlock(&rec_lock);
}

static void target_line(VkExec *ex, const VkExecTarget *t, const char *tag, int n) {
    char fn[16], name[160];
    if (!t->image) {
        fprintf(passes_f, "  %s: nvn %#x target %d %ux%u: not renderable, left out\n", tag, t->tex->d.format,
                t->tex->d.target, t->tex->d.width, t->tex->d.height);
        return;
    }
    snprintf(name, sizeof name, "pass_%04d_%s_%ux%u_%d_%s", n, tag, t->width, t->height, (int)t->format,
             fmt_name(t->format, fn));
    // a block view: the plane's blocks, read as the view format (one texel per block)
    uint32_t tw = t->tex->d.width >> t->level ? t->tex->d.width >> t->level : 1;
    uint32_t th = t->tex->d.height >> t->level ? t->tex->d.height >> t->level : 1;
    (void)n;
    const char *why = pass_skip ? pass_skip : t->samples > 1 ? "multisampled" :
                      t->blocks ? copy_region(ex, t->image, t->format, t->aspect, t->level, t->is3d ? 0 : t->base_layer,
                                              (VkOffset3D){0, 0, t->is3d ? (int32_t)t->base_layer : 0},
                                              (VkExtent3D){tw, th, 1}, t->width, t->height, 1, name) :
                      t->is3d ? "3D slice (the volume is dumped as lut<id> where it is sampled)" :
                      copy_out(ex, t->image, t->format, t->aspect, t->level, t->base_layer, t->width, t->height, 1, name);
    fprintf(passes_f, "  %s: nvn %#x %s(%d) %ux%u level %u layers %u+%u samples %u addr %#llx image %p -> %s.png%s%s\n",
            tag, t->nvn_format, fmt_name(t->format, fn), (int)t->format, t->width, t->height, t->level, t->base_layer,
            t->layers, t->samples, (unsigned long long)(uintptr_t)t->tex->mem, (void *)t->image, name,
            why ? " skipped: " : "", why ? why : "");
}

uint64_t targets_sig(const VkExec *ex) {
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < 9; i++) {
        const VkExecTarget *t = i < 8 ? &ex->color[i] : &ex->depth;
        uint64_t v[3] = {(uint64_t)(uintptr_t)t->image, t->level, t->base_layer};
        for (int k = 0; k < 3; k++) h = (h ^ v[k]) * 1099511628211ull;
    }
    return h;
}

// Sets pass_skip for the pass or dispatch being logged and counts it when it copies.
static void pass_decide(int same_targets) {
    if (frame_now() != dump_frame) copied = 0;
    pass_skip = luts_only ? "volumes only" : copied >= MAX_PASSES ? "pass cap" : same_targets ? "same targets as the previous pass" : NULL;
    if (!pass_skip) copied++;
}

// Pipeline statistics per render pass of the dumped frame (stats.txt): input primitives, vertex shader invocations,
// primitives into and out of clipping, fragment shader invocations. Where a pass loses its geometry (the race's main
// pass rasterized nothing, run-race15) shows as the first zero.
#define STAT_MAX 512
static VkQueryPool stat_pool;
static int stat_n, stat_open = -1, stat_pass[STAT_MAX];

void vk_dump_pass_begin(VkExec *ex) {
    plat_mutex_lock(&rec_lock);
    if (active() && frame_now() == dump_frame && stat_n < STAT_MAX) {
        VkCtx *c = vk_ctx();
        if (!stat_pool) {
            VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                        .queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS, .queryCount = STAT_MAX,
                                        .pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT |
                                                              VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT |
                                                              VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT |
                                                              VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT |
                                                              VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT};
            if (vkCreateQueryPool(c->device, &qi, NULL, &stat_pool) != VK_SUCCESS) stat_pool = VK_NULL_HANDLE;
            if (stat_pool) vkResetQueryPool(c->device, stat_pool, 0, STAT_MAX);
        }
        if (stat_pool) {
            stat_open = stat_n++;
            vkCmdBeginQuery(ex->cb, stat_pool, (uint32_t)stat_open, 0);
        }
    }
    plat_mutex_unlock(&rec_lock);
}

void vk_dump_pass_close(VkExec *ex) {   // before vkCmdEndRendering
    plat_mutex_lock(&rec_lock);
    if (stat_open >= 0) {
        vkCmdEndQuery(ex->cb, stat_pool, (uint32_t)stat_open);
        stat_pass[stat_open] = seq;   // the number vk_dump_pass_end gives this pass
        stat_open = -1;
    }
    plat_mutex_unlock(&rec_lock);
}

static void stats_write(void) {
    if (!stat_pool || !stat_n) return;
    VkCtx *c = vk_ctx();
    uint64_t (*r)[5] = calloc((size_t)stat_n, sizeof *r);
    char path[600];
    snprintf(path, sizeof path, "%s/stats.txt", dump_dir);
    FILE *f = fopen(path, "w");
    if (r && f && vkGetQueryPoolResults(c->device, stat_pool, 0, (uint32_t)stat_n, sizeof *r * (size_t)stat_n, r, sizeof *r,
                                        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS) {
        fprintf(f, "pass prims vs_invocations clip_in clip_out fs_invocations\n");
        for (int i = 0; i < stat_n; i++)
            fprintf(f, "%04d %llu %llu %llu %llu %llu\n", stat_pass[i], (unsigned long long)r[i][0],
                    (unsigned long long)r[i][1], (unsigned long long)r[i][2], (unsigned long long)r[i][3],
                    (unsigned long long)r[i][4]);
    }
    if (f) fclose(f);
    free(r);
}

void vk_dump_pass_end(VkExec *ex) {
    plat_mutex_lock(&rec_lock);
    if (!active()) goto out;
    int n = seq++;
    uint64_t sig = targets_sig(ex);
    pass_decide(skip_runs && sig == last_targets);
    last_targets = sig;
    pass_skip = draws_pass_end(sig, pass_skip);
    fprintf(passes_f, "pass %04d: render pass, draws %d, area %ux%u layers %u, fence %llu, frame %llu\n", n, pass_draws,
            ex->pass_w, ex->pass_h, ex->pass_layers, (unsigned long long)ex->fence, (unsigned long long)frame_now());
    for (int i = 0; i < 8; i++) {
        if (!ex->color[i].tex) continue;
        char tag[8];
        snprintf(tag, sizeof tag, "t%d", i);
        target_line(ex, &ex->color[i], tag, n);
    }
    if (ex->depth.tex) target_line(ex, &ex->depth, "td", n);
    for (int i = 0; i < nents; i++) {
        fprintf(passes_f, "  pipe %d (draws %d): %s", i, ents[i].draws, ents[i].text);
        free(ents[i].text);
    }
    if (ents_over) {
        fprintf(passes_f, "  ... %d more draws with other pipeline/texture sets; their fs:", ents_over);
        for (int k = 0; k < n_over_fs; k++) fprintf(passes_f, " %016llx*%d", (unsigned long long)over_fs[k].fs, over_fs[k].draws);
        fprintf(passes_f, "\n");
    }
    if (!pass_skip || luts_only) flush_luts(ex, n);
    else nluts = 0;
    buf_snap(ex, n);
    fflush(passes_f);
    nents = ents_over = pass_draws = n_over_fs = 0;
out:
    plat_mutex_unlock(&rec_lock);
}

// MKW_TRACE_3D=1 (diagnosis): the first dispatch that writes each 3D storage image is logged with its frame, size,
// shader and caller (the race's colour-grading LUT is written in some earlier frame, not in the one dumped).
void vk_trace_3d_writes(VkExec *ex, const GfxShader *cs, const VkPipeInfo *info, const p_dispatch *d) {
    static int on = -1;
    static VkImage seen[256];
    static int nseen;
    if (on < 0) on = cfg_get("MKW_TRACE_3D") && atoi(cfg_get("MKW_TRACE_3D"));
    if (!on) return;
    for (uint32_t i = 0; i < info->n_bindings; i++) {
        const VkPipeBinding *bd = &info->bindings[i];
        if (!bd->res || bd->kind != GFX_RES_IMG || !(bd->res->flags & 4) || bd->type != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
            continue;
        uint64_t h = handle_of(ex, 5, bd->res->slot, 1);
        view_t v;
        int hv = 0;
        const VkTexImage *t = h ? vk_texture_by_id(NVN_HANDLE_TEX(h), &v, &hv) : NULL;
        if (!t || t->type != VK_IMAGE_TYPE_3D) continue;
        int k = 0;
        while (k < nseen && seen[k] != t->image) k++;
        if (k < nseen || nseen == 256) continue;
        seen[nseen++] = t->image;
        uint64_t words = 0, sh = 0xcbf29ce484222325ull;
        const uint8_t *b = (const uint8_t *)gfx_shader_spirv(cs, &words);
        for (uint64_t j = 0; j < words * 4; j++) sh = (sh ^ b[j]) * 0x100000001b3ull;
        rt_log("vk: frame %llu: 3D image %ux%ux%u fmt %d (tex %u, view %d) written by cs %016llx slot %d, dispatch %dx%dx%d, "
               "caller %#llx", (unsigned long long)frame_now(), t->width, t->height, t->depth, (int)t->format,
               (unsigned)NVN_HANDLE_TEX(h), hv, (unsigned long long)sh, bd->res->slot, d->x, d->y, d->z,
               (unsigned long long)d->caller);
    }
}

void vk_dump_dispatch(VkExec *ex, const GfxShader *cs, VkPipeline p, const VkPipeInfo *info, const p_dispatch *d) {
    (void)p;
    plat_mutex_lock(&rec_lock);
    if (!active()) goto out;
    int n = seq++;
    pass_decide(0);
    last_targets = 0;
    Str b = {0};
    describe_bindings(&b, ex, info);
    fprintf(passes_f, "pass %04d: dispatch %dx%dx%d cs %016llx prog %llu caller %#llx, fence %llu\n%s", n, d->x, d->y,
            d->z, (unsigned long long)shader_hash(cs), (unsigned long long)(ex->prog ? ex->prog->serial : 0),
            (unsigned long long)d->caller, (unsigned long long)ex->fence, b.s ? b.s : "");
    free(b.s);
    for (uint32_t i = 0; i < info->n_bindings && !pass_skip; i++) {   // storage images the dispatch writes
        const VkPipeBinding *bd = &info->bindings[i];
        if (!bd->res || bd->kind != GFX_RES_IMG || !(bd->res->flags & 4) || bd->type != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
            continue;
        uint64_t h = handle_of(ex, 5, bd->res->slot, 1);
        view_t v;
        int hv = 0;
        const VkTexImage *t = h ? vk_texture_by_id(NVN_HANDLE_TEX(h), &v, &hv) : NULL;
        if (!t) continue;
        uint32_t lv = hv && v.set_levels ? v.base_level : 0, layer = hv && v.set_layers ? v.min_layer : 0;
        if (lv >= t->levels || layer >= t->layers) continue;
        uint32_t w = t->width >> lv ? t->width >> lv : 1, hh = t->height >> lv ? t->height >> lv : 1;
        uint32_t dd = t->type == VK_IMAGE_TYPE_3D ? (t->depth >> lv ? t->depth >> lv : 1) : 1;
        char fn[16], name[160];
        snprintf(name, sizeof name, "pass_%04d_img%d_%ux%u%s_%d_%s", n, bd->res->slot, w, hh, dd > 1 ? "x3d" : "",
                 (int)t->format, fmt_name(t->format, fn));
        if (t->type == VK_IMAGE_TYPE_3D) {   // a written volume: this copy replaces the sampled-3D one
            for (int k = 0; k < nluts_done; k++)
                if (luts_done[k] == t->image) luts_done[k] = VK_NULL_HANDLE;
        }
        const char *why = t->samples > 1 ? "multisampled" :
                          copy_out(ex, t->image, t->format, t->aspect, lv, layer, w, hh, dd, name);
        fprintf(passes_f, "  writes img slot %d level %u layer %u depth %u -> %s.png%s%s\n", bd->res->slot, lv, layer,
                dd, name, why ? " skipped: " : "", why ? why : "");
    }
    if (!pass_skip || luts_only) flush_luts(ex, n);
    else nluts = 0;
    buf_snap(ex, n);
    fflush(passes_f);
out:
    plat_mutex_unlock(&rec_lock);
}

static const char *tex_desc(const tex_t *t, char *buf, size_t n) {
    if (!t) return "null";
    snprintf(buf, n, "addr %#llx nvn-fmt %#x %ux%ux%u", (unsigned long long)(uintptr_t)t->mem, t->d.format, t->d.width,
             t->d.height, t->d.depth);
    return buf;
}

static void region_desc(const NVNcopyRegion *r, char *buf, size_t n) {
    snprintf(buf, n, "(%d,%d,%d %dx%dx%d)", r->xoffset, r->yoffset, r->zoffset, r->width, r->height, r->depth);
}

void vk_dump_copy(VkExec *ex, uint32_t op, const void *payload) {
    (void)ex;
    plat_mutex_lock(&rec_lock);
    if (!active()) goto out;
    char a[96], b[96], ra[64], rb[64];
    switch (op) {
    case OP_COPY_B2B: {
        const p_copy_b2b *p = payload;
        fprintf(passes_f, "copy after pass %d: buffer %#llx -> buffer %#llx, %llu bytes\n", seq - 1,
                (unsigned long long)p->src, (unsigned long long)p->dst, (unsigned long long)p->size);
        break;
    }
    case OP_COPY_B2T: case OP_COPY_T2B: {
        const p_copy_b2t *p = payload;
        region_desc(&p->r, ra, sizeof ra);
        fprintf(passes_f, "copy after pass %d: %s buffer %#llx %s texture %s region %s flags %#x\n", seq - 1,
                op == OP_COPY_B2T ? "buffer->texture" : "texture->buffer", (unsigned long long)p->src,
                op == OP_COPY_B2T ? "->" : "<-", tex_desc(p->tex, a, sizeof a), ra, p->flags);
        break;
    }
    case OP_COPY_T2T: {
        const p_copy_t2t *p = payload;
        region_desc(&p->sr, ra, sizeof ra);
        region_desc(&p->dr, rb, sizeof rb);
        fprintf(passes_f, "copy after pass %d: texture %s %s -> texture %s %s flags %#x\n", seq - 1,
                tex_desc(p->src, a, sizeof a), ra, tex_desc(p->dst, b, sizeof b), rb, p->flags);
        break;
    }
    case OP_CLEAR_TEX: {
        const p_clear_tex *p = payload;
        region_desc(&p->r, ra, sizeof ra);
        fprintf(passes_f, "clear after pass %d: texture %s region %s mask %#x\n", seq - 1, tex_desc(p->tex, a, sizeof a),
                ra, p->mask);
        break;
    }
    default: break;
    }
out:
    plat_mutex_unlock(&rec_lock);
}

static void make_dirs(char *path) { plat_file_mkdir_p(path); }

static void open_dir(const char *d) {
    snprintf(dump_dir, sizeof dump_dir, "%s", d);
    make_dirs(dump_dir);
    char path[600];
    snprintf(path, sizeof path, "%s/passes.txt", dump_dir);
    passes_f = fopen(path, "w");
    snprintf(path, sizeof path, "%s/images.txt", dump_dir);
    images_f = fopen(path, "w");
    if (!passes_f || !images_f) rt_fatal("vk_dump: cannot write to %s", dump_dir);
}

// MKW_CTL (ctl.c): dumps the next frame into d (ubo: MKW_DUMP_UBO; luts: only the sampled 3D volumes and small
// textures, a few MB instead of hundreds). 0 while an earlier dump is not written yet.
int vk_dump_rearm(const char *d, int ubo, int luts, int log_n) {
    plat_mutex_lock(&rec_lock);
    int busy = passes_f != NULL;
    if (!busy) {
        open_dir(d);
        seq = copied = nluts = nluts_done = stat_n = 0, stat_open = -1, png_bytes = 0;
        draws_reset();
        dump_ubo = ubo, luts_only = luts, log_from = 0, finished = 0;
        if (stat_pool) vkResetQueryPool(vk_ctx()->device, stat_pool, 0, STAT_MAX);
        dump_frame = frame_now() + 1 + (uint64_t)(log_n > 0 ? log_n : 0);
        if (log_n > 0) log_from = frame_now() + 1;
        vk_dump_armed = 1;
        rt_log("vk_dump: frame %llu goes to %s", (unsigned long long)dump_frame, dump_dir);
    }
    plat_mutex_unlock(&rec_lock);
    return !busy;
}

void vk_dump_init(void) {
    const char *tu = cfg_get("MKW_TRACE_UBO");
    if (tu && sscanf(tu, "%llx:%u", (unsigned long long *)&tu_fs, &tu_rows) >= 1) {
        const char *o = cfg_get("MKW_TRACE_UBO_OUT");
        if (!(tu_f = fopen(o && *o ? o : "ubo_trace.txt", "w"))) rt_fatal("ubo trace: cannot write %s", o && *o ? o : "ubo_trace.txt");
        vk_dump_armed = 1;
        rt_log("ubo trace: fs %016llx, %u rows of each block, to %s", (unsigned long long)tu_fs, tu_rows, o && *o ? o : "ubo_trace.txt");
    }
    const char *wt = cfg_get("MKW_DUMP_TEX");   // read without MKW_DUMP too: MKW_CTL dumps use it
    for (char *q = (char *)wt; q && *q && nwant_tex < 16; q += *q == ',') {
        uint32_t *w = want_tex[nwant_tex++];
        w[0] = (uint32_t)strtoul(q, &q, 10);
        w[1] = *q == 'x' ? (uint32_t)strtoul(q + 1, &q, 10) : 0;
    }
    draws_init();
    const char *e = cfg_get("MKW_DUMP");
    if (!e || !*e || !(dump_frame = strtoull(e, NULL, 10))) return;
    const char *lf = cfg_get("MKW_DUMP_LOG");
    if (lf && *lf) log_from = strtoull(lf, NULL, 10);
    const char *sr = cfg_get("MKW_DUMP_SKIP_RUNS");
    skip_runs = sr && *sr && *sr != '0';
    const char *du = cfg_get("MKW_DUMP_UBO");
    dump_ubo = du && *du && *du != '0';
    const char *d = cfg_get("MKW_DUMP_DIR");
    open_dir(d && *d ? d : "dump");
    char path[600];
    const char *bf = cfg_get("MKW_DUMP_BUF");
    if (bf && *bf) {
        char *q;
        bd_addr = strtoull(bf, &q, 0);
        if (*q == ':') bd_size = strtoull(q + 1, &q, 0);
        if (*q == ':') bd_off = strtoull(q + 1, &q, 0);
        if (*q == ':') bd_stride = strtoull(q + 1, &q, 0) & ~3ull;
        if (*q == ':') bd_w = (uint32_t)strtoul(q + 1, &q, 0);
        if (*q == ':') bd_h = (uint32_t)strtoul(q + 1, &q, 0);
        if (!bd_w || !bd_h) bd_stride = 0;
        snprintf(path, sizeof path, "%s/bufs.txt", dump_dir);
        if (bd_size && !(bufs_f = fopen(path, "w"))) bd_size = 0;
    }
    vk_dump_armed = 1;
    rt_log("vk_dump: frame %llu goes to %s", (unsigned long long)dump_frame, dump_dir);
}

void vk_dump_present(uint64_t frame) {
    if (frame != dump_frame) return;
    plat_mutex_lock(&rec_lock);
    finished = 1;
    plat_mutex_unlock(&rec_lock);
    vk_sync_finish();   // the frame's copies are converted and written
    stats_write();
    plat_mutex_lock(&img_lock);
    fclose(images_f);
    images_f = NULL;
    if (bufs_f) fclose(bufs_f), bufs_f = NULL;
    plat_mutex_unlock(&img_lock);
    fclose(passes_f);
    passes_f = NULL;
    rt_log("vk_dump: frame %llu: %d passes and dispatches, about %llu MB of PNGs in %s", (unsigned long long)frame, seq,
           (unsigned long long)(png_bytes >> 20), dump_dir);
}
