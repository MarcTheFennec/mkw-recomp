// NVN2 (Switch 2) entry points the NVN1 front of totk-pc-recomp lacks, met by MKW at nvnDeviceGetProcAddress.
//
// Two kinds:
// - ProgramBuilder accessors are plain loads and stores on an object only the game touches, so the proc address handed
//   out is the driver's own (recompiled) function, read from the sdk's proc table (nvn2_sdk_proc; offsets of each game version's sdk,
//   as tools/gpu/nvn2_procs.py reads them). Device, queue and window objects hold the front's own state, so their new
//   functions are host functions here.
// - HDR, ray tracing and fast-clear registration are optional console features, off (README "Decisiones"): their
//   setters do nothing and their getters report SDR, no ray-tracing memory and nothing registered.
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "nvn_api.h"
#include "cfg.h"
#include "kernel.h"
#include "platform.h"
#include "sha256.h"
#include "nvn_tex.h"
#include "version.h"

// The driver's proc table, per sdk build (version.c, the rows of tools/gpu/nvn2_procs.py TABLES): the names without
// "nvn", sorted, as u32 offsets from the name pool, the highest index of the driver's binary search, and the u64
// pointers, relocated by the loader (1.0.0: sdk+0xb74c26, 0xb79b90, 0x2b9, 0xd95168 from sdk+0x5b5a70 and 0x5ffad0).
#define SDK_NAME_POOL (rt_version()->sdk_name_pool)
#define SDK_NAME_OFFSETS (rt_version()->sdk_name_offsets)
#define SDK_LAST_INDEX (rt_version()->sdk_last_index)
#define SDK_POINTERS (rt_version()->sdk_pointers)

static uint64_t sdk_base(void) {
    for (int i = 0; i < ldr_count(); i++)
        if (!strcmp(ldr_module(i)->name, "sdk")) return ldr_module(i)->base;
    rt_fatal("nvn2: sdk module not loaded");
}

// The driver's function for an NVN2 proc name, or 0.
uint64_t nvn2_sdk_proc(const char *name) {
    if (strncmp(name, "nvn", 3)) return 0;
    uint64_t base = sdk_base();
    int lo = 0, hi = SDK_LAST_INDEX;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const char *entry = (const char *)(uintptr_t)(base + SDK_NAME_POOL + LD32(base + SDK_NAME_OFFSETS + 4ull * mid));
        int cmp = strcmp(name + 3, entry);
        if (!cmp) return LD64(base + SDK_POINTERS + 8ull * mid);
        if (cmp < 0) hi = mid - 1;
        else lo = mid + 1;
    }
    return 0;
}

// Names whose driver function is a pure accessor on a game-only object (checked by disassembly, 30/10/2026).
int nvn2_use_sdk_proc(const char *name) { return !strncmp(name, "nvnProgramBuilder", 17); }

// MKW_SPIRV_DUMP set: who creates a program whose control has bytes 0x18-0x2f at zero, the programs the game generates
// in partida, and the label the game gives it. The first 16 of each.
static int generated_stage(int n, const NVNshaderData *sd) {
    static const uint8_t zero[0x18];
    const char *dump = cfg_get("MKW_SPIRV_DUMP");
    if (!dump || !*dump || !sd || n < 0 || n > 6) return -1;
    for (int i = 0; i < n; i++)
        if (sd[i].control && !memcmp((const uint8_t *)sd[i].control + 0x18, zero, sizeof zero)) return i;
    return -1;
}

// MKW_GEN_WATCH=1: who writes the generated programs. At the first one, a 32 MiB window around its code is made
// read-only (committed read-write pages only); each later write there logs the host instruction (addr2line on
// build/mkw names the lifted guest function, f_7000...) and makes that page writable again.
static uint64_t gw_lo, gw_hi;
static volatile int32_t gw_logged;
static int gen_watch_fault(uint64_t addr, uint64_t rip) {
    if (addr < gw_lo || addr >= gw_hi) return 0;
    if (plat_inc32(&gw_logged) <= 20000) {
        char line[128];
        int k = snprintf(line, sizeof line, "[host] gen watch: write at %#llx from host rip %#llx\n",
                         (unsigned long long)addr, (unsigned long long)rip);
        if (k > 0) (void)!write(2, line, (size_t)k);
    }
    return plat_mem_protect(addr & ~0xfffull, 0x1000, PLAT_PROT_RW);
}

static void gen_watch_start(uint64_t code) {
    static volatile int32_t started;
    const char *e = cfg_get("MKW_GEN_WATCH");
    if (!e || !atoi(e) || plat_inc32(&started) != 1) return;
    gw_lo = (code & ~0xfffffull) - (16ull << 20), gw_hi = gw_lo + (32ull << 20);
    plat_mem_watch_hook(gen_watch_fault);
    uint64_t a = gw_lo, end, pages = 0;
    while (a < gw_hi) {   // only the committed read-write runs: holes stay as they are
        int st = plat_mem_state(a, gw_hi, &end);
        if (st == PLAT_PROT_RW && plat_mem_protect(a, end - a, PLAT_PROT_READ)) pages += (end - a) >> 12;
        a = end > a ? end : a + 0x1000;
    }
    rt_log("nvn2: gen watch over %#llx-%#llx: %llu pages read-only", (unsigned long long)gw_lo,
           (unsigned long long)gw_hi, (unsigned long long)pages);
}

// MKW_WRITE_WATCH=<addr>:<bytes> (diagnosis): who writes that guest range (the race's uniform rows holding NaN, run-race33).
// Its pages are made read-only at every present (nvn2_write_watch_arm); the first writes to each page in a frame log the
// address and host instruction (addr2line on build/mkw names the lifted function) and make the page writable again.
// A page the runtime also write-tracks loses that tracking while watched: for diagnosis only.
static uint64_t ww_lo, ww_hi;
static volatile int32_t ww_logged;
static int write_watch_fault(uint64_t addr, uint64_t rip) {
    if (addr < (ww_lo & ~0xfffull) || addr >= ww_hi) return gw_hi ? gen_watch_fault(addr, rip) : 0;
    if (plat_inc32(&ww_logged) <= 4000) {
        char line[160];
        int k = snprintf(line, sizeof line, "[host] write watch: frame %llu write at %#llx (+%#llx) from host rip %#llx\n",
                         (unsigned long long)gfx_counters.frames, (unsigned long long)addr,
                         (unsigned long long)(addr - ww_lo), (unsigned long long)rip);
        if (k > 0) (void)!write(2, line, (size_t)k);
    }
    return plat_mem_protect(addr & ~0xfffull, 0x1000, PLAT_PROT_RW);
}

void nvn2_write_watch_arm(void) {
    static int state = -1;   // -1 unread, 0 off, 1 on
    if (state < 0) {
        const char *e = cfg_get("MKW_WRITE_WATCH");
        unsigned long long a = 0, n = 0;
        state = e && sscanf(e, "%llx:%llx", &a, &n) == 2 && n;
        if (state) ww_lo = a, ww_hi = a + n, plat_mem_watch_hook(write_watch_fault);
        if (state) rt_log("nvn2: write watch over %#llx-%#llx", a, a + n);
    }
    if (state) plat_mem_protect(ww_lo & ~0xfffull, ((ww_hi + 0xfff) & ~0xfffull) - (ww_lo & ~0xfffull), PLAT_PROT_READ);
}

void nvn2_note_generated(const Cpu *c, int n, const NVNshaderData *sd) {
    static volatile int32_t seen;
    int i = generated_stage(n, sd);
    if (i < 0 || plat_inc32(&seen) > 16) return;
    rt_log("nvn2: generated program, stage %d of %d, code %#llx control %p", i, n, (unsigned long long)sd[i].data,
           (const void *)sd[i].control);
    kern_backtrace(c);
    gen_watch_start(sd[i].data);
}

void nvn2_note_label(const prog_t *o, const char *label) {
    static volatile int32_t seen;
    int i = generated_stage((int)o->nstage, o->sd);
    if (i < 0 || !label || plat_inc32(&seen) > 64) return;
    rt_log("nvn2: generated program \"%.120s\", stage %d of %u, code %#llx", label, i, o->nstage,
           (unsigned long long)o->sd[i].data);
}

// ---------- ProgramBuilder -> program (NVN2 creates programs from a builder)
// Builder layout as the driver's accessors use it: +0x08 device, +0x28 stage count, +0x30 stage data.
// NVNboolean nvnProgramInitializeFromBuilder(NVNprogram *p, const NVNprogramBuilder *b)
NVN_API(nvnProgramInitializeFromBuilder) {
    NVN_BLOCKING();
    prog_t *o = arg_p(c, 0);
    uint64_t b = arg_x(c, 1);
    int n = (int)LD32(b + 0x28);
    const NVNshaderData *sd = (const NVNshaderData *)(uintptr_t)LD64(b + 0x30);
    memset(o, 0, sizeof *o);
    o->magic = NVN_PROG_MAGIC;
    // NVNshaderData {data, control} as in NVN1 (hypothesis): every control must carry the NVN control magic.
    for (int i = 0; i < n && sd; i++)
        if (!sd[i].control || LD32((uint64_t)(uintptr_t)sd[i].control) != 0x98761234u)
            rt_fatal("nvn2: ProgramInitializeFromBuilder stage %d of %d: control %p without the NVN magic", i, n,
                     (const void *)sd[i].control);
    nvn2_note_generated(c, n, sd);
    ret_x(c, nvn_prog_set_shaders(o, n, sd));
}

// ---------- Device
// int nvnDeviceGetFlags(const NVNdevice *d)
NVN_API(nvnDeviceGetFlags) { ret_x(c, ((const dev_t_ *)arg_p(c, 0))->flags); }
// NVNboolean nvnDeviceRegisterFastClear*(NVNdevice *d, ...): nothing registered, the game clears the usual way.
NVN_API(nvnDeviceRegisterFastClearColorIndex) { ret_x(c, 0); }
NVN_API(nvnDeviceRegisterFastClearColoriIndex) { ret_x(c, 0); }
NVN_API(nvnDeviceRegisterFastClearColoruiIndex) { ret_x(c, 0); }
NVN_API(nvnDeviceRegisterFastClearStencil) { ret_x(c, 0); }

// ---------- Queue: ray tracing off, one queue
NVN_API(nvnQueueBuilderSetPrimaryQueue) { (void)c; }
NVN_API(nvnQueueBuilderSetRayTracingMemorySize) { (void)c; }
// NVNboolean nvnQueueBuilderGetRayTracingMemorySize(const NVNqueueBuilder *b, int *size)
NVN_API(nvnQueueBuilderGetRayTracingMemorySize) {
    int32_t *size = arg_p(c, 1);
    if (size) *size = 0;
    ret_x(c, 0);
}
NVN_API(nvnQueueGetTotalRayTracingMemoryUsed) { ret_x(c, 0); }

// ---------- Window: HDR off, the default (SDR) color space
NVN_API(nvnWindowBuilderSetColorSpace) { (void)c; }
NVN_API(nvnWindowBuilderGetColorSpace) { ret_x(c, 0); }
NVN_API(nvnWindowBuilderSetHdrDisplayMetadata) { (void)c; }
NVN_API(nvnWindowBuilderSetHdrContentMetadata) { (void)c; }
NVN_API(nvnWindowSetHdrDisplayMetadata) { (void)c; }
NVN_API(nvnWindowSetHdrContentMetadata) { (void)c; }
// Getters write floats through their pointer arguments; with HDR off every value is 0.
static void zero_float_outs(Cpu *c, int first, int count) {
    for (int i = first; i < first + count; i++) {
        float *p = arg_p(c, i);
        if (p) *p = 0;
    }
}
NVN_API(nvnWindowBuilderGetHdrContentMetadata) { zero_float_outs(c, 1, 2); }
NVN_API(nvnWindowGetHdrContentMetadata) { zero_float_outs(c, 1, 2); }
// Display metadata: 10 values (primaries, white point, luminance), the last two through the stack.
NVN_API(nvnWindowBuilderGetHdrDisplayMetadata) { zero_float_outs(c, 1, 10); }
NVN_API(nvnWindowGetHdrDisplayMetadata) { zero_float_outs(c, 1, 10); }

// ---------- Sparse textures
// NVNtextureSparseTileLayout as the driver writes it (sdk+0x59c2e0): levels with whole tiles, then the tile size in
// texels. A tile is 64 KiB; its shape in blocks is the standard sparse block shape (the same as Vulkan's), scaled by the
// block size of compressed formats (the driver multiplies by it the same way). Multisampled sparse textures stop the
// run: the driver divides by a sample grid this does not model, and MKW has not been seen to make one.
typedef struct { int32_t levels, width, height, depth; } sparse_layout;

static void sparse_tile_layout(const nvn_texdesc *d, sparse_layout *out) {
    static const int32_t shape2d[5][2] = {{256, 256}, {256, 128}, {128, 128}, {128, 64}, {64, 64}};
    static const int32_t shape3d[5][3] = {{64, 32, 32}, {32, 32, 32}, {32, 32, 16}, {32, 16, 16}, {16, 16, 16}};
    int bw, bh, bpb = nvn_fmt_bpb(d->format, &bw, &bh), k = 0;
    if (!bpb) rt_fatal("nvn2: sparse tile layout of unknown format %#x", d->format);
    if (d->samples > 1) rt_fatal("nvn2: sparse tile layout of a %d-sample texture", d->samples);
    while ((1 << k) < bpb) k++;
    if (k > 4) rt_fatal("nvn2: sparse tile layout of %d-byte blocks", bpb);
    int is3d = d->target == NVN_TEXTURE_TARGET_3D;
    out->width = (is3d ? shape3d[k][0] : shape2d[k][0]) * bw;
    out->height = (is3d ? shape3d[k][1] : shape2d[k][1]) * bh;
    out->depth = is3d ? shape3d[k][2] : 1;
    // Levels from the base while the level still holds a whole tile; the rest is the mip tail.
    out->levels = 0;
    for (uint32_t l = 0; l < d->levels; l++) {
        uint32_t w = d->width >> l ? d->width >> l : 1, h = d->height >> l ? d->height >> l : 1;
        uint32_t z = is3d ? (d->depth >> l ? d->depth >> l : 1) : 1;
        if (w < (uint32_t)out->width || h < (uint32_t)out->height || z < (uint32_t)out->depth) break;
        out->levels++;
    }
}

void nvn_sparse_tile_layout(const nvn_texdesc *d, int32_t out[4]) { sparse_tile_layout(d, (sparse_layout *)out); }

// void nvnTextureGetSparseTileLayout(const NVNtexture *t, NVNtextureSparseTileLayout *out)
NVN_API(nvnTextureGetSparseTileLayout) {
    sparse_layout *out = arg_p(c, 1);
    sparse_tile_layout(&((const tex_t *)arg_p(c, 0))->d, out);
    NVN_LOG("TextureGetSparseTileLayout: %d levels, tile %dx%dx%d", out->levels, out->width, out->height, out->depth);
}
// void nvnTextureBuilderGetSparseTileLayout(const NVNtextureBuilder *b, NVNtextureSparseTileLayout *out)
NVN_API(nvnTextureBuilderGetSparseTileLayout) {
    sparse_tile_layout(&((const tex_builder_t *)arg_p(c, 0))->d, arg_p(c, 1));
}

// ---------- Command buffers
// void nvnCommandBufferSetRecordingMode(NVNcommandBuffer *cb, int mode): the driver only stores the mode (+0x18);
// the front records the same way in any mode. The first value is logged until a mode is seen to matter.
NVN_API(nvnCommandBufferSetRecordingMode) {
    static int logged;
    if (!__atomic_exchange_n(&logged, 1, __ATOMIC_RELAXED))
        rt_log("nvn2: CommandBufferSetRecordingMode %d (ignored)", (int)arg_x(c, 1));
}

// ---------- State hints and optional features with no effect on the Vulkan back end
// Each logs its first call (with its first argument after the object) so a value that turns out to matter shows up.
// Ray tracing and variable rate shading are optional features, off (README "Decisiones"); the rest are hints the
// driver uses for its own scheduling or debugging.
#define NVN2_IGNORED(n)                                                                                             \
    NVN_API(n) {                                                                                                     \
        static int logged;                                                                                           \
        if (!__atomic_exchange_n(&logged, 1, __ATOMIC_RELAXED))                                                      \
            rt_log("nvn2: " #n " (%#llx) ignored", (unsigned long long)arg_x(c, 1));                                 \
    }
NVN2_IGNORED(nvnCommandBufferSetComputeOverlapMode)
NVN2_IGNORED(nvnCommandBufferSetComputeEnable)
NVN2_IGNORED(nvnCommandBufferSetComputeEnableConditional)
NVN2_IGNORED(nvnCommandBufferSetCopyEnable)
NVN2_IGNORED(nvnCommandBufferSetCopyEnableConditional)
NVN2_IGNORED(nvnCommandBufferSetConservativeRasterMode)
NVN2_IGNORED(nvnCommandBufferSetGeometryProcessingMode)
NVN2_IGNORED(nvnCommandBufferSetRepresentativeFragmentTest)
NVN2_IGNORED(nvnCommandBufferSetExclusiveScissor)
NVN2_IGNORED(nvnCommandBufferSetExclusiveScissors)
NVN2_IGNORED(nvnCommandBufferEnableExclusiveScissor)
NVN2_IGNORED(nvnCommandBufferPushDebugGroupGraphics)
NVN2_IGNORED(nvnCommandBufferPushDebugGroupCompute)
NVN2_IGNORED(nvnCommandBufferPopDebugGroupGraphics)
NVN2_IGNORED(nvnCommandBufferPopDebugGroupCompute)
NVN2_IGNORED(nvnCommandBufferInsertDebugMarkerGraphics)
NVN2_IGNORED(nvnCommandBufferInsertDebugMarkerCompute)
// New in 1.8.0's sdk (tools/gpu/gen_nvn2_procs.py): scheduling hints for the console's GPU, nothing a Vulkan queue
// takes. The other procs new there keep their weak stub, which stops with the proc's name if the game calls it.
NVN2_IGNORED(nvnCommandBufferSetGraphicsWorkloadPriority)
NVN2_IGNORED(nvnCommandBufferSetComputeWorkloadPriority)
NVN2_IGNORED(nvnCommandBufferSetResourceOccupancy)
NVN2_IGNORED(nvnMultisampleStateSetCoverageTruncationEnable)
NVN_API(nvnMultisampleStateGetCoverageTruncationEnable) { ret_x(c, 0); }
// Variable rate shading (optional, off)
NVN2_IGNORED(nvnCommandBufferBindShadingRateImage)
NVN2_IGNORED(nvnCommandBufferSetShadingRatePaletteMode)
NVN2_IGNORED(nvnCommandBufferShadingRateImagePalette)
// Transform feedback, tessellation, stencil culling and viewport swizzles: not used in what MKW draws so far;
// debugging and GL interop
NVN2_IGNORED(nvnCommandBufferBeginTransformFeedback)
NVN2_IGNORED(nvnCommandBufferSetStencilCullCriteria)
NVN2_IGNORED(nvnCommandBufferSetViewportSwizzles)
NVN2_IGNORED(nvnCommandBufferSetInnerTessellationLevels)
NVN2_IGNORED(nvnTextureBuilderSetGLTextureName)
NVN2_IGNORED(nvnDeviceMessageBufferSetDefaults)
NVN2_IGNORED(nvnProgramSetSampleShading)
// Ray tracing (optional, off): nothing is bound, built or traced
NVN2_IGNORED(nvnCommandBufferBindAccelerationStructure)
NVN2_IGNORED(nvnCommandBufferBindAccelerationStructures)
NVN2_IGNORED(nvnCommandBufferBindAccelerationStructuresDeferred)
NVN2_IGNORED(nvnCommandBufferBuildAccelerationStructures)
NVN2_IGNORED(nvnCommandBufferCompactAccelerationStructure)
NVN2_IGNORED(nvnCommandBufferCopyAccelerationStructure)
NVN2_IGNORED(nvnCommandBufferRelocateAccelerationStructure)
NVN2_IGNORED(nvnCommandBufferDispatchRays)
NVN2_IGNORED(nvnCommandBufferDispatchRaysIndirect)

// ---------- Shader programs (NVN2 control section and code)
// The control section is 0xd8 bytes; the code size is at +0x44 (SPH included). Graphics code starts with a 0x80-byte
// SPH whose type says the stage; compute code has none. The key of a program is sha256(control + code cut as
// tools/shaders/extract.py trim() cuts it: at the first all-zero 16-byte word, or at another SPH right after NOP
// padding), the name tools/shaders/translate.py gives its prepared SPIR-V.
#define NVN2_CONTROL 0xd8u
#define NVN2_SPH 0x80u
#define NVN2_NOP_LOW16 0x7918u

static int nvn2_sph_stage(const uint8_t *code) {
    uint32_t w;
    memcpy(&w, code, 4);
    if ((w & 0x1f) != 1 && (w & 0x1f) != 2) return -1;
    if (((w >> 5) & 0x1f) != 4) return -1;
    uint32_t t = (w >> 10) & 0xf;   // 1 VS .. 5 PS
    return t >= 1 && t <= 5 ? (int)t - 1 : -1;
}

int nvn2_program_stage(const uint8_t *control, const uint8_t *code) {
    // NVNshaderStage (VS 0, FS 1, GS 2, TCS 3, TES 4, CS 5, TASK 6, MESH 7) is the u16 at control+0x56: it matches the
    // extraction's stage in all 11 561 programs (31/10/2026: vs 3 546, ps 7 262, cs 728, task 7, mesh 18). The SPH type
    // cannot tell them apart: the task programs and 6 mesh programs carry a VS header, the other 12 mesh a type-3 one.
    (void)code;
    uint16_t st;
    memcpy(&st, control + 0x56, 2);
    return st < 8 ? st : 5;
}

static uint32_t program_length_scan(const uint8_t *code, uint32_t size);

// The scan reads the whole program in 16-byte steps, and the replay asks again whenever a program gets a new serial
// (nvnProgramSetShaders: vk_exec_draw.c find_stage), 0.6-0.9 % of a race's CPU samples (rt-k0). Kept per code address
// and size, with the first 64 bytes after the header as a fingerprint against another program loaded at the same place.
#define PLEN_N 4096
static struct { const uint8_t *code; uint32_t size, len; uint64_t fp; } plen[PLEN_N];
static PlatMutex plen_lock = PLAT_MUTEX_INIT;

static uint64_t program_fp(const uint8_t *code, uint32_t size) {
    uint32_t at = nvn2_sph_stage(code) >= 0 ? NVN2_SPH : 0;
    uint64_t h = 0xcbf29ce484222325ull, w;
    for (uint32_t i = at; i + 8 <= at + 64 && i + 8 <= size; i += 8) memcpy(&w, code + i, 8), h = (h ^ w) * 0x100000001b3ull;
    return h;
}

uint32_t nvn2_program_length(const uint8_t *control, const uint8_t *code) {
    uint32_t size;
    memcpy(&size, control + 0x44, 4);
    uint64_t fp = program_fp(code, size);
    typeof(plen[0]) *e = &plen[((uintptr_t)code >> 4) * 0x9E3779B97F4A7C15ull >> 52];
    plat_mutex_lock(&plen_lock);
    int hit = e->code == code && e->size == size && e->fp == fp;
    uint32_t len = e->len;
    plat_mutex_unlock(&plen_lock);
    if (hit) return len;
    len = program_length_scan(code, size);
    plat_mutex_lock(&plen_lock);
    e->code = code, e->size = size, e->fp = fp, e->len = len;
    plat_mutex_unlock(&plen_lock);
    return len;
}

static uint32_t program_length_scan(const uint8_t *code, uint32_t size) {
    int graphics = nvn2_sph_stage(code) >= 0;
    static const uint8_t zero[16];
    for (uint32_t at = graphics ? NVN2_SPH : 0; at + 16 <= size; at += 16) {
        if (!memcmp(code + at, zero, 16)) return at;
        uint16_t prev;
        if (at > (graphics ? NVN2_SPH : 0u)) {
            memcpy(&prev, code + at - 16, 2);
            if (prev == NVN2_NOP_LOW16 && nvn2_sph_stage(code + at) >= 0) return at;
        }
    }
    return size;
}

void nvn2_program_key(const uint8_t *control, const uint8_t *code, uint8_t key[32]) {
    uint32_t len = nvn2_program_length(control, code);
    uint8_t *buf = malloc(NVN2_CONTROL + len);
    if (!buf) rt_fatal("nvn2: program key buffer");
    memcpy(buf, control, NVN2_CONTROL);
    memcpy(buf + NVN2_CONTROL, code, len);
    rt_sha256(buf, NVN2_CONTROL + len, key);
    free(buf);
}

// ---------- Mesh and task draws: skipped until the translator does task/mesh programs (ISBERD/ISBEWR) and the back end
// has VK_EXT_mesh_shader pipelines. MKW run44 calls DrawMeshTasks after its
// first present; skipping keeps the game going without those draws.
static void mesh_skipped(const char *name) {
    static volatile int32_t seen;
    if (plat_inc32(&seen) <= 8) rt_log("nvn2: %s skipped (mesh shaders not supported yet)", name);
}
// nvnCommandBufferDrawMeshTasks is recorded in nvn_api_cmd.c (OP_DRAW_MESH).
NVN_API(nvnCommandBufferDrawMeshTasks3D) { (void)c; mesh_skipped("DrawMeshTasks3D"); }
NVN_API(nvnCommandBufferDrawMeshTasksIndirect) { (void)c; mesh_skipped("DrawMeshTasksIndirect"); }
NVN_API(nvnCommandBufferDrawMeshTasks3DIndirect) { (void)c; mesh_skipped("DrawMeshTasks3DIndirect"); }
NVN_API(nvnCommandBufferMultiDrawMeshTasksIndirect) { (void)c; mesh_skipped("MultiDrawMeshTasksIndirect"); }
NVN_API(nvnCommandBufferMultiDrawMeshTasks3DIndirect) { (void)c; mesh_skipped("MultiDrawMeshTasks3DIndirect"); }
NVN_API(nvnCommandBufferMultiDrawMeshTasksIndirectCount) { (void)c; mesh_skipped("MultiDrawMeshTasksIndirectCount"); }
NVN_API(nvnCommandBufferMultiDrawMeshTasks3DIndirectCount) { (void)c; mesh_skipped("MultiDrawMeshTasks3DIndirectCount"); }

// nvnCommandBufferPush{Graphics,Compute}Constants[Deferred] are recorded in nvn_api_cmd.c (OP_PUSH_CONST): the
// shaders read them as constant bank c24 (translated reflections with slot 24; 5 programs in 1.8.0).

// ---------- The rest of the procs MKW's code reads (tools/gpu/nvn_usage.py) that the totk-pc-recomp front lacked. Signatures
// from nvn.h (NVN1) unless noted.
// Builder getters: the field the matching setter stored.
NVN_API(nvnMemoryPoolBuilderGetDevice) { ret_x(c, (uint64_t)(uintptr_t)((const pool_builder_t *)arg_p(c, 0))->dev); }
NVN_API(nvnBufferBuilderGetDevice) { ret_x(c, (uint64_t)(uintptr_t)((const buf_builder_t *)arg_p(c, 0))->dev); }
NVN_API(nvnDeviceBuilderGetFlags) { ret_x(c, ((const dev_builder_t *)arg_p(c, 0))->flags); }
// void nvnTexturePoolRegisterImage(const NVNtexturePool *tp, int id, const NVNtexture *t, const NVNtextureView *v):
// image and texture descriptors share the pool's index space, so the slot now holds this texture.
NVN_API(nvnTexturePoolRegisterImage) { gfx_backend()->texture_register((int)arg_x(c, 1), arg_p(c, 2), arg_p(c, 3)); }
// void nvnSamplerPoolRegisterSamplerBuilder(const NVNsamplerPool *sp, int id, const NVNsamplerBuilder *b)
NVN_API(nvnSamplerPoolRegisterSamplerBuilder) {
    gfx_backend()->sampler_register((int)arg_x(c, 1), &((const samp_builder_t *)arg_p(c, 2))->p);
}
// NVNboolean nvnQueueWaitSync(NVNqueue *q, const NVNsync *s): one queue, whose commands run in order, so the wait is
// already met when the queue reaches it.
NVN_API(nvnQueueWaitSync) { ret_x(c, 1); }
// NVNdebugDomainId nvnDeviceGenerateDebugDomainId(const NVNdevice *d, const char *name): any distinct id.
NVN_API(nvnDeviceGenerateDebugDomainId) {
    static volatile int32_t next;
    (void)c;
    ret_x(c, (uint64_t)plat_inc32(&next));
}

// ---------- Native images (NVN2 in 1.8.0's sdk): a texture over an image another service owns, here the GameChat
// cameras (main+0xd32270: 384x216 RGBA8 sRGB, three per camera, from nn::chat::Get*CameraImageHandle). The game calls
// these driver exports by name, not through GetProcAddress, and aborts when Initialize fails (main+0xd32338). No chat
// is ever started (ov_chat.c), so each image is a texture of the camera's size with no storage, never sampled. The
// handle carries that size (ov_chat.c: width << 16 | height).
typedef struct { uint32_t magic, format, perm, handle; } nimage_t;
void nvnp_nvnTextureInitialize(Cpu *c);   // nvn_api_obj.c
#define NIMAGE_MAGIC 0x474d494eu   // "NIMG"
static void nimg_defaults(Cpu *c) { memset(arg_p(c, 0), 0, sizeof(nimage_t)); }
static void nimg_device(Cpu *c) { (void)c; }
static void nimg_format(Cpu *c) { ((nimage_t *)arg_p(c, 0))->format = (uint32_t)arg_x(c, 1); }
static void nimg_perm(Cpu *c) { ((nimage_t *)arg_p(c, 0))->perm = (uint32_t)arg_x(c, 1); }
static void nimg_handle(Cpu *c) { ((nimage_t *)arg_p(c, 0))->handle = (uint32_t)arg_x(c, 1); }
// NVNboolean nvnNativeImageInitialize(NVNnativeImage *, const NVNnativeImageBuilder *)
static void nimg_init(Cpu *c) {
    nimage_t *o = arg_p(c, 0);
    *o = *(const nimage_t *)arg_p(c, 1);
    o->magic = NIMAGE_MAGIC;
    ret_x(c, 1);
}
static void nimg_fini(Cpu *c) { ((nimage_t *)arg_p(c, 0))->magic = 0; }
// NVNboolean nvnTextureInitializeFromNativeImage(NVNtexture *, NVNnativeImage *): the front's own texture initialize
// with a builder of that size and format and no storage.
static void nimg_texture(Cpu *c) {
    const nimage_t *n = arg_p(c, 1);
    if (n->magic != NIMAGE_MAGIC) rt_fatal("nvn2: TextureInitializeFromNativeImage with an uninitialized native image");
    static tex_builder_t b;   // the game thread only; Initialize copies it
    memset(&b, 0, sizeof b);
    b.d.swizzle_r = 2, b.d.swizzle_g = 3, b.d.swizzle_b = 4, b.d.swizzle_a = 5;
    b.d.target = NVN_TEXTURE_TARGET_2D;
    b.d.width = n->handle >> 16 ? n->handle >> 16 : 1;
    b.d.height = n->handle & 0xffff ? n->handle & 0xffff : 1;
    b.d.depth = b.d.levels = 1;
    b.d.format = (int)n->format;
    uint64_t x1 = c->x[1];
    c->x[1] = (uint64_t)(uintptr_t)&b;
    nvnp_nvnTextureInitialize(c);   // ret_x: 1
    c->x[1] = x1;
}
HOST_OVERRIDE("_Z32nvnNativeImageBuilderSetDefaultsP21NVNnativeImageBuilder", nimg_defaults);
HOST_OVERRIDE("_Z30nvnNativeImageBuilderSetDeviceP21NVNnativeImageBuilderP9NVNdevice", nimg_device);
HOST_OVERRIDE("_Z30nvnNativeImageBuilderSetFormatP21NVNnativeImageBuilder9NVNformat", nimg_format);
HOST_OVERRIDE("_Z40nvnNativeImageBuilderSetMemoryPermissionP21NVNnativeImageBuilder30NVNnativeImageMemoryPermission",
              nimg_perm);
HOST_OVERRIDE("_Z41nvnNativeImageBuilderSetNativeImageHandleP21NVNnativeImageBuilderN2nn2gs17NativeImageHandleE",
              nimg_handle);
HOST_OVERRIDE("_Z24nvnNativeImageInitializeP14NVNnativeImagePK21NVNnativeImageBuilder", nimg_init);
HOST_OVERRIDE("_Z22nvnNativeImageFinalizeP14NVNnativeImage", nimg_fini);
HOST_OVERRIDE("_Z35nvnTextureInitializeFromNativeImageP10NVNtextureP14NVNnativeImage", nimg_texture);
