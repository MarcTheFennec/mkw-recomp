// mkw --probe-vkexec: the Vulkan replay, through gfx_vulkan_backend. Sync part: empty
// submissions get consecutive fence values; fence_wait and fence_done agree; completion callbacks run in submit order on
// the completion thread; a guest-memory word a callback writes is visible once fence_wait returns; finish drains every
// submission; a present of a cleared vk_texture.c image (layout GENERAL) reads back exactly through the swapchain
// capture. Clear part: a hand-built op stream (SET_RT to an RGBA8 and a D32 texture, whole-target clears,
// then scissored clears) through submit reads back exactly with read_texels. Rtstats part: MKW_RTSTATS's rules on
// cleared targets (test_rtstats). Target history part: which targets count as rendered recently (test_recent).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "nvn_api.h"
#include "nvn_tex.h"
#include "vk.h"
#include "vk_exec.h"
#include "vk_exec_internal.h"
#include "platform/platform.h"

#define SUBMITS 100
#define TAIL 10
#define TEX_W 256
#define TEX_H 144
#define CHECK(cond, ...)                                                                                                \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("probe-vkexec: FAILED: ");                                                                          \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

static uint32_t main_tid;
static uint64_t ran[SUBMITS + TAIL + 8];
static long nran, on_main;

static void note(void *arg) {   // completion thread
    if (nran < (long)(sizeof ran / sizeof ran[0])) ran[nran] = (uint64_t)(uintptr_t)arg;
    nran++;
    if (plat_thread_id() == main_tid) on_main++;
}

typedef struct { volatile uint32_t *word; uint32_t value; } WordWrite;
static void write_word(void *arg) {
    const WordWrite *w = arg;
    *w->word = w->value;
}

static long ran_now(void) { return __atomic_load_n(&nran, __ATOMIC_ACQUIRE); }

// Target history (vk_exec_rt.c): which draws wait for a pending pipeline as one-shot and which are skipped. A target
// rendered every frame and one whose layers take turns (one of 12 a frame) are recent; one rendered once long ago, or
// never, is not.
static int test_recent(void) {
    VkExecTarget every = {.image = (VkImage)(uintptr_t)0x1000}, turn = {.image = (VkImage)(uintptr_t)0x2000, .base_layer = 5};
    VkExecTarget once = {.image = (VkImage)(uintptr_t)0x3000}, never = {.image = (VkImage)(uintptr_t)0x4000};
    tfr_note(&every, 99), tfr_note(&every, 100);
    tfr_note(&turn, 88), tfr_note(&turn, 100);
    tfr_note(&once, 40), tfr_note(&once, 100);
    CHECK(tfr_recent(&every, 100), "a target rendered in the previous frame is not recent");
    CHECK(tfr_recent(&turn, 100), "a layer rendered 12 frames before (one layer of 12 a frame) is not recent");
    CHECK(tfr_recent(&turn, 112), "a layer rendered 12 frames before, not yet in this frame, is not recent");
    CHECK(!tfr_recent(&once, 100), "a target last rendered 60 frames before is recent");
    CHECK(!tfr_recent(&once, 200), "a target last rendered 100 frames before is recent");
    CHECK(!tfr_recent(&never, 100), "a target never rendered is recent");
    return 0;
}

static int test_sync(const GfxBackend *b, uint8_t *guest, uint64_t *first, uint64_t *last) {
    // An empty recording as the front end hands it over: {OP_BEGIN, OP_END}.
    static op_hdr empty[2] = {{OP_BEGIN, sizeof(op_hdr)}, {OP_END, sizeof(op_hdr)}};
    uint64_t handles[2] = {(uint64_t)(uintptr_t)empty, (uint64_t)(uintptr_t)empty};
    uint64_t prev = b->fence_done();
    *first = prev + 1;
    for (int i = 0; i < SUBMITS; i++) {
        vk_completion_add(prev + 1, note, (void *)(uintptr_t)(prev + 1));   // queued before the value exists, as vk_exec.c does
        uint64_t f = b->submit(i % 3 ? handles : NULL, i % 3);
        CHECK(f == prev + 1, "submission %d got fence %llu after %llu", i, (unsigned long long)f, (unsigned long long)prev);
        CHECK(b->fence_done() <= f, "fence_done %llu ahead of the newest submission %llu",
              (unsigned long long)b->fence_done(), (unsigned long long)f);
        CHECK(!b->fence_wait(f + 1, 0), "fence_wait of a value never submitted returned 1");
        if (i % 10 == 9) {
            CHECK(b->fence_wait(f, 5000000000ull), "fence_wait(%llu) timed out", (unsigned long long)f);
            CHECK(b->fence_done() >= f, "fence_done %llu after fence_wait(%llu)", (unsigned long long)b->fence_done(),
                  (unsigned long long)f);
            CHECK(ran_now() == i + 1, "%ld callbacks ran after fence_wait of submission %d", ran_now(), i);
        }
        prev = f;
    }
    // A guest-memory word written by a completion callback is visible to the waiter.
    volatile uint32_t *word = (volatile uint32_t *)(guest + 0x1000);
    *word = 0;
    static WordWrite ww;
    ww = (WordWrite){word, 0x5EC0DE01u};
    vk_completion_add(prev + 1, write_word, &ww);
    uint64_t f = b->submit(NULL, 0);
    CHECK(f == prev + 1, "word submission got fence %llu", (unsigned long long)f);
    CHECK(b->fence_wait(f, 5000000000ull), "fence_wait of the word submission timed out");
    CHECK(*word == 0x5EC0DE01u, "guest word is %#x after fence_wait", *word);
    prev = f;
    // finish drains everything.
    for (int i = 0; i < TAIL; i++) {
        vk_completion_add(prev + 1, note, (void *)(uintptr_t)(prev + 1));
        prev = b->submit(handles, 2);
    }
    b->finish();
    CHECK(b->fence_done() == prev, "fence_done %llu after finish, newest %llu", (unsigned long long)b->fence_done(),
          (unsigned long long)prev);
    CHECK(ran_now() == SUBMITS + TAIL, "%ld of %d callbacks ran after finish", ran_now(), SUBMITS + TAIL);
    for (int i = 0; i < SUBMITS + TAIL; i++) {
        uint64_t want = *first + (uint64_t)i + (i >= SUBMITS);   // the word submission sits between the two runs
        CHECK(ran[i] == want, "callback %d ran for fence %llu, expected %llu", i, (unsigned long long)ran[i],
              (unsigned long long)want);
    }
    CHECK(!on_main, "%ld callbacks ran on the submitting thread", on_main);
    *last = prev;
    return 0;
}

static uint8_t to_byte(float f) { return (uint8_t)(f * 255.0f + 0.5f); }

static int test_present(const GfxBackend *b, uint8_t *guest, int *w_out, int *h_out) {
    pool_t pool = {.magic = NVN_POOL_MAGIC, .flags = 0x22, .mem = guest + 0x100000, .size = 0x100000,
                   .game_mem = guest + 0x100000};
    CHECK(b->pool_init(&pool), "pool_init of a CPU-visible pool");
    tex_t t;
    memset(&t, 0, sizeof t);
    t.magic = NVN_TEX_MAGIC;
    t.d.target = NVN_TEXTURE_TARGET_2D, t.d.format = NVN_FORMAT_RGBA8, t.d.width = TEX_W, t.d.height = TEX_H;
    t.d.depth = 1, t.d.levels = 1;
    nvn_tex_layout(&t.d, &t.L);
    CHECK(t.L.size <= pool.size, "texture storage %#llx exceeds the pool", (unsigned long long)t.L.size);
    t.pool = &pool, t.off = 0, t.mem = pool.mem, t.serial = 1;
    const VkTexImage *img = vk_texture_get(&t);
    CHECK(img && img->format == VK_FORMAT_R8G8B8A8_UNORM, "vk_texture_get of an RGBA8 texture");
    vk_texture_gpu_written(&t);   // the clear below is its content, not the storage
    const float rgba[4] = {0.2f, 0.4f, 0.6f, 1.0f};
    VkClearColorValue cv;
    memcpy(cv.float32, rgba, sizeof cv.float32);
    VkImageSubresourceRange rng = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkCommandBuffer cb = vk_upload_begin();
    vkCmdClearColorImage(cb, img->image, VK_IMAGE_LAYOUT_GENERAL, &cv, 1, &rng);
    vk_upload_end();

    uint64_t frames = gfx_stats().frames;
    // the exact colour check needs the bilinear blit: FSR's approximate reciprocals move a flat colour by 1/255
    plat_setenv("MKW_UPSCALER", "bilinear");
    vk_present_capture(1);
    const int crop[4] = {0, 0, TEX_W, TEX_H};
    b->present(&t, crop, 0);
    b->finish();   // the queued present is submitted and, with capture on, read back
    const uint8_t *px;
    int w, h;
    VkFormat fmt;
    CHECK(vk_present_captured(&px, &w, &h, &fmt), "the present was not captured");
    CHECK(gfx_stats().frames == frames + 1, "frame counter %llu after one present from %llu",
          (unsigned long long)gfx_stats().frames, (unsigned long long)frames);
    uint8_t want[4] = {0, 0, 0, 255};
    if (fmt == VK_FORMAT_B8G8R8A8_UNORM) want[0] = to_byte(rgba[2]), want[1] = to_byte(rgba[1]), want[2] = to_byte(rgba[0]);
    else if (fmt == VK_FORMAT_R8G8B8A8_UNORM) want[0] = to_byte(rgba[0]), want[1] = to_byte(rgba[1]), want[2] = to_byte(rgba[2]);
    else CHECK(0, "swapchain format %d is not UNORM RGBA/BGRA", (int)fmt);
    uint32_t wv;
    memcpy(&wv, want, 4);
    const uint32_t *p = (const uint32_t *)px;
    for (long k = 0, n = (long)w * h; k < n; k++)
        CHECK(p[k] == wv, "presented pixel %ld is %08x, expected %08x", k, p[k], wv);
    vk_present_capture(0);
    b->texture_release(&t);
    b->pool_fini(&pool);
    *w_out = w, *h_out = h;
    return 0;
}

// ---- clear part: a hand-built op stream through submit, read back with read_texels.

static uint8_t *put(uint8_t *p, uint32_t op, const void *payload, uint32_t n) {
    op_hdr h = {op, (uint32_t)((sizeof h + n + 7) & ~7u)};
    memset(p, 0, h.size);
    memcpy(p, &h, sizeof h);
    if (n) memcpy(p + sizeof h, payload, n);
    return p + h.size;
}

static void init_tex(tex_t *t, pool_t *pool, int64_t off, int format) {
    memset(t, 0, sizeof *t);
    t->magic = NVN_TEX_MAGIC;
    t->d.target = NVN_TEXTURE_TARGET_2D, t->d.format = format, t->d.width = TEX_W, t->d.height = TEX_H;
    t->d.depth = 1, t->d.levels = 1;
    nvn_tex_layout(&t->d, &t->L);
    t->pool = pool, t->off = off, t->mem = pool->mem + off, t->serial = 100 + (uint64_t)off;
}

// Pixel of the RGBA8 target after the stream: the scissored second clear inside the rectangle, the first elsewhere.
#define SC_X 40
#define SC_Y 24
#define SC_W 64
#define SC_H 32

static int test_clear(const GfxBackend *b, uint8_t *guest, int *n_inside) {
    pool_t pool = {.magic = NVN_POOL_MAGIC, .flags = 0x22, .mem = guest + 0x400000, .size = 0x200000,
                   .game_mem = guest + 0x400000};
    CHECK(b->pool_init(&pool), "pool_init of a CPU-visible pool for the clear part");
    static tex_t color, depth;   // payload texture pointers live as long as the submission
    init_tex(&color, &pool, 0, NVN_FORMAT_RGBA8);
    init_tex(&depth, &pool, 0x100000, NVN_FORMAT_DEPTH32F);
    CHECK(color.L.size <= 0x100000 && depth.L.size <= 0x100000, "clear-part textures exceed their pool halves");

    static _Alignas(8) uint8_t stream[4096];
    uint8_t *p = stream;
    p = put(p, OP_BEGIN, NULL, 0);
    p_set_rt rt;
    memset(&rt, 0, sizeof rt);
    rt.n = 1, rt.color[0] = &color, rt.depth = &depth;
    p = put(p, OP_SET_RT, &rt, sizeof rt);
    const p_clear_color c0 = {0, 0xf, {0.2f, 0.4f, 0.6f, 1.0f}};
    p = put(p, OP_CLEAR_COLOR, &c0, sizeof c0);   // whole target outside a pass: vkCmdClearColorImage
    const p_clear_ds d0 = {0.5f, 1, 0, 0xff};
    p = put(p, OP_CLEAR_DS, &d0, sizeof d0);      // vkCmdClearDepthStencilImage
    const p_scissor sc = {SC_X, SC_Y, SC_W, SC_H};
    p = put(p, OP_SCISSOR, &sc, sizeof sc);
    const p_clear_color c1 = {0, 0xf, {1.0f, 0.0f, 0.0f, 1.0f}};
    p = put(p, OP_CLEAR_COLOR, &c1, sizeof c1);   // scissored: a pass and vkCmdClearAttachments
    const p_clear_ds d1 = {0.25f, 1, 0, 0xff};
    p = put(p, OP_CLEAR_DS, &d1, sizeof d1);
    const p_scissor full = {0, 0, 1 << 20, 1 << 20};
    p = put(p, OP_SCISSOR, &full, sizeof full);   // leaves the replay's scissor unbounded for later submissions
    p = put(p, OP_END, NULL, 0);
    CHECK(p <= stream + sizeof stream, "op stream overflow");

    uint64_t handle = (uint64_t)(uintptr_t)stream;
    uint64_t f = b->submit(&handle, 1);
    CHECK(b->fence_wait(f, 5000000000ull), "fence_wait of the clear submission timed out");

    static uint32_t px[TEX_W * TEX_H];
    static float dpx[TEX_W * TEX_H];
    NVNcopyRegion r = {0, 0, 0, TEX_W, TEX_H, 1};
    b->read_texels(&color, NULL, &r, (uint8_t *)px, TEX_W * 4, TEX_W * TEX_H * 4);
    b->read_texels(&depth, NULL, &r, (uint8_t *)dpx, TEX_W * 4, TEX_W * TEX_H * 4);
    const uint8_t out_b[4] = {to_byte(0.2f), to_byte(0.4f), to_byte(0.6f), 255}, in_b[4] = {255, 0, 0, 255};
    uint32_t out_v, in_v;
    memcpy(&out_v, out_b, 4), memcpy(&in_v, in_b, 4);
    int inside = 0;
    for (int y = 0; y < TEX_H; y++)
        for (int x = 0; x < TEX_W; x++) {
            int in = x >= SC_X && x < SC_X + SC_W && y >= SC_Y && y < SC_Y + SC_H;
            inside += in;
            uint32_t want = in ? in_v : out_v;
            float dwant = in ? 0.25f : 0.5f;
            CHECK(px[y * TEX_W + x] == want, "colour texel (%d,%d) is %08x, expected %08x", x, y, px[y * TEX_W + x], want);
            CHECK(dpx[y * TEX_W + x] == dwant, "depth texel (%d,%d) is %g, expected %g", x, y, dpx[y * TEX_W + x], dwant);
        }
    b->texture_release(&color);
    b->texture_release(&depth);
    b->pool_fini(&pool);
    *n_inside = inside;
    return 0;
}

void vk_exec_draw_shutdown(void);   // vk_exec_view.c: the stand-in resources; the device must be idle
void vk_exec_xfer_shutdown(void);   // vk_exec_xfer.c: the occlusion query pool; the device must be idle

// ---- xfer part: copies, texture clear, event and counter through submit.

#define XF_W 64
#define XF_H 64
#define XF_BYTES (XF_W * XF_H * 4)

// Texture in the xfer pool at `off`.
static int xf_tex(tex_t *t, pool_t *pool, int64_t off, uint64_t serial) {
    init_tex(t, pool, off, NVN_FORMAT_RGBA8);
    t->d.width = XF_W, t->d.height = XF_H;
    nvn_tex_layout(&t->d, &t->L);
    t->serial = serial;
    return t->L.size <= 0x20000;
}

static int xf_submit(const GfxBackend *b, uint8_t *stream, uint8_t *end) {
    end = put(end, OP_END, NULL, 0);
    (void)stream;
    uint64_t handle = (uint64_t)(uintptr_t)stream;
    uint64_t f = b->submit(&handle, 1);
    return b->fence_wait(f, 5000000000ull);
}

static int test_xfer(const GfxBackend *b, uint8_t *guest, int *bytes) {
    pool_t pool = {.magic = NVN_POOL_MAGIC, .flags = 0x22, .mem = guest + 0x800000, .size = 0x100000,
                   .game_mem = guest + 0x800000};
    CHECK(b->pool_init(&pool), "pool_init of a CPU-visible pool for the xfer part");
    uint8_t *m = pool.mem;
    static tex_t ta, tb, tc;
    CHECK(xf_tex(&ta, &pool, 0x40000, 201) && xf_tex(&tb, &pool, 0x60000, 202) && xf_tex(&tc, &pool, 0x80000, 203),
          "xfer textures exceed their pool slots");
    // Linear buffers: B2B source 0x0, target 0x1000 (4 KiB); texture source 0x2000, read-backs 0x6000 and 0xA000.
    for (int i = 0; i < 4096; i++) m[i] = (uint8_t)(i * 7 + 3), m[0x1000 + i] = 0;
    uint32_t *img = (uint32_t *)(m + 0x2000);
    for (int i = 0; i < XF_W * XF_H; i++) img[i] = 0x01010101u * (uint32_t)(i & 0xff) + (uint32_t)i * 0x00010000u;
    memset(m + 0x6000, 0, XF_BYTES), memset(m + 0xA000, 0, XF_BYTES);
    volatile uint32_t *event = (volatile uint32_t *)(m + 0xE000);
    NVNcounterData *ctr = (NVNcounterData *)(m + 0xE100), *sp = (NVNcounterData *)(m + 0xE200);
    *event = 0, ctr->counter = 99, ctr->timestamp = 0;
    sp->counter = 99, sp->timestamp = 0;

    static _Alignas(8) uint8_t stream[4096];
    uint8_t *p = stream;
    p = put(p, OP_BEGIN, NULL, 0);
    const p_copy_b2b bb = {(uint64_t)(uintptr_t)m, (uint64_t)(uintptr_t)(m + 0x1000), 4096};
    p = put(p, OP_COPY_B2B, &bb, sizeof bb);
    p_copy_b2t up;
    memset(&up, 0, sizeof up);
    up.src = (uint64_t)(uintptr_t)(m + 0x2000), up.tex = &ta, up.r = (NVNcopyRegion){0, 0, 0, XF_W, XF_H, 1};
    p = put(p, OP_COPY_B2T, &up, sizeof up);
    p_copy_b2t down = up;
    down.src = (uint64_t)(uintptr_t)(m + 0x6000);
    p = put(p, OP_COPY_T2B, &down, sizeof down);
    p_copy_t2t tt;
    memset(&tt, 0, sizeof tt);
    tt.src = &ta, tt.dst = &tb, tt.sr = tt.dr = up.r;
    p = put(p, OP_COPY_T2T, &tt, sizeof tt);
    down.tex = &tb, down.src = (uint64_t)(uintptr_t)(m + 0xA000);
    p = put(p, OP_COPY_T2B, &down, sizeof down);
    p_clear_tex ct;
    memset(&ct, 0, sizeof ct);
    ct.tex = &tc, ct.r = (NVNcopyRegion){0, 0, 0, XF_W, XF_H, 1}, ct.mask = 0xf;
    ct.c.f[0] = 0.2f, ct.c.f[1] = 0.4f, ct.c.f[2] = 0.6f, ct.c.f[3] = 1.0f;
    p = put(p, OP_CLEAR_TEX, &ct, sizeof ct);
    const p_signal_event ev1 = {event, NVN_EVENT_SIGNAL_MODE_WRITE, 7}, ev2 = {event, NVN_EVENT_SIGNAL_MODE_ADD, 5};
    p = put(p, OP_SIGNAL_EVENT, &ev1, sizeof ev1);
    p = put(p, OP_SIGNAL_EVENT, &ev2, sizeof ev2);
    const p_report_counter rc = {NVN_COUNTER_TYPE_TIMESTAMP, (uint64_t)(uintptr_t)ctr};
    p = put(p, OP_REPORT_COUNTER, &rc, sizeof rc);
    // Samples passed: a reset, no draw, a report: {0, time} through the occlusion-query path.
    const p_reset_counter rs = {NVN_COUNTER_TYPE_SAMPLES_PASSED};
    p = put(p, OP_RESET_COUNTER, &rs, sizeof rs);
    const p_report_counter rsp = {NVN_COUNTER_TYPE_SAMPLES_PASSED, (uint64_t)(uintptr_t)sp};
    p = put(p, OP_REPORT_COUNTER, &rsp, sizeof rsp);
    CHECK(p <= stream + sizeof stream - sizeof(op_hdr), "xfer op stream overflow");
    CHECK(xf_submit(b, stream, p), "fence_wait of the xfer submission timed out");

    for (int i = 0; i < 4096; i++) CHECK(m[0x1000 + i] == m[i], "B2B byte %d is %02x, expected %02x", i, m[0x1000 + i], m[i]);
    for (int i = 0; i < XF_W * XF_H; i++) {
        CHECK(((uint32_t *)(m + 0x6000))[i] == img[i], "B2T then T2B texel %d is %08x, expected %08x", i,
              ((uint32_t *)(m + 0x6000))[i], img[i]);
        CHECK(((uint32_t *)(m + 0xA000))[i] == img[i], "T2T then T2B texel %d is %08x, expected %08x", i,
              ((uint32_t *)(m + 0xA000))[i], img[i]);
    }
    static uint32_t px[XF_W * XF_H];
    const NVNcopyRegion r = {0, 0, 0, XF_W, XF_H, 1};
    b->read_texels(&ta, NULL, &r, (uint8_t *)px, XF_W * 4, XF_BYTES);
    for (int i = 0; i < XF_W * XF_H; i++) CHECK(px[i] == img[i], "read_texels of the copied texture, texel %d is %08x", i, px[i]);
    b->read_texels(&tc, NULL, &r, (uint8_t *)px, XF_W * 4, XF_BYTES);
    const uint8_t cl[4] = {to_byte(0.2f), to_byte(0.4f), to_byte(0.6f), 255};
    uint32_t clv;
    memcpy(&clv, cl, 4);
    for (int i = 0; i < XF_W * XF_H; i++) CHECK(px[i] == clv, "CLEAR_TEX texel %d is %08x, expected %08x", i, px[i], clv);
    CHECK(*event == 12, "event word is %u after WRITE 7 and ADD 5", *event);
    // (MKW_REPORT_AT_SUBMIT, the default: the front end writes timestamp reports at the submit, which this stream skips)
    if (nvn_report_at_submit())
        CHECK(ctr->counter == 99 && ctr->timestamp == 0, "a timestamp report the replay left alone is {%llu, %llu}",
              (unsigned long long)ctr->counter, (unsigned long long)ctr->timestamp);
    else
        CHECK(ctr->counter == 0 && ctr->timestamp != 0, "counter report is {%llu, %llu}", (unsigned long long)ctr->counter,
              (unsigned long long)ctr->timestamp);
    CHECK(sp->counter == 0 && sp->timestamp != 0, "samples-passed report after a reset and no draw is {%llu, %llu}",
          (unsigned long long)sp->counter, (unsigned long long)sp->timestamp);
    b->texture_release(&ta);
    b->texture_release(&tb);
    b->texture_release(&tc);
    b->pool_fini(&pool);
    *bytes = 4096 + 2 * XF_BYTES;
    return 0;
}

// c0 as vk_c0_fill builds it (CPU only): compute storage and uniform tables, dispatch size, unbound slots zero; the
// vertex stage's point size clamp and base instance.
static int test_c0(void) {
    static p_bind_buf ssbo[16], ubo[16];
    static uint8_t bank[VK_C0_BYTES];
    ssbo[1] = (p_bind_buf){5, 1, 0x12340000, 0x400};
    ubo[4] = (p_bind_buf){5, 4, 0x55660000, 0x100};
    const uint32_t dim[3] = {3, 2, 1};
    uint32_t u[4];
    uint64_t a;
    vk_c0_fill(bank, 5, ssbo, ubo, dim, 7, NULL, NULL, NULL);
    memcpy(u, bank + 0x4, 12);
    CHECK(u[0] == 3 && u[1] == 2 && u[2] == 1, "c0 dispatch size at 0x4 is %u %u %u", u[0], u[1], u[2]);
    memcpy(&a, bank + 0x320, 8), memcpy(u, bank + 0x328, 4);
    CHECK(a == 0x12340000 && u[0] == 0x400, "c0 0x320 (storage slot 1) is {%#llx, %#x}", (unsigned long long)a, u[0]);
    memcpy(&a, bank + 0x250, 8), memcpy(u, bank + 0x258, 4);
    CHECK(a == 0x55660000 && u[0] == 0x100, "c0 0x250 (uniform slot 4) is {%#llx, %#x}", (unsigned long long)a, u[0]);
    for (int i = 0; i < 16; i++) CHECK(!bank[0x310 + i], "c0 0x310 (unbound storage slot 0) byte %d is %02x", i, bank[0x310 + i]);
    CHECK(!bank[0x644] && !bank[0x610], "c0 of a compute stage has graphics words");
    // NVN2 compute: the program's packed table (2 uniform, 2 storage, 1 texture, 1 image entries from 0x10, no push
    // constant words: vk_c0_fill reads the fifth count too, and a 4-byte table let it read whatever byte came next)
    static uint64_t tex[128], img[32];
    static const uint8_t table[5] = {2, 2, 1, 1, 0};
    tex[0] = 0xab00012, img[0] = 0xcd00034, ubo[1] = (p_bind_buf){5, 1, 0x77880000, 0x40};
    vk_c0_fill(bank, 5, ssbo, ubo, dim, 7, table, tex, img);
    memcpy(&a, bank + 0x20, 8);
    CHECK(a == 0x77880000, "packed c0: uniform slot 1 at 0x20 is %#llx", (unsigned long long)a);
    memcpy(&a, bank + 0x40, 8), memcpy(u, bank + 0x48, 4);
    CHECK(a == 0x12340000 && u[0] == 0x400, "packed c0: storage slot 1 at 0x40 is {%#llx, %#x}", (unsigned long long)a, u[0]);
    memcpy(&a, bank + 0x50, 8);
    CHECK(a == 0xab00012, "packed c0: texture slot 0 at 0x50 is %#llx", (unsigned long long)a);
    memcpy(&a, bank + 0x58, 8);
    CHECK(a == 0xcd00034, "packed c0: image slot 0 at 0x58 is %#llx", (unsigned long long)a);
    CHECK(!bank[0x320], "packed c0 has NVN1's storage table");
    ubo[1] = (p_bind_buf){0};
    vk_c0_fill(bank, 0, ssbo, ubo, dim, 61, NULL, NULL, NULL);
    float ps;
    memcpy(&ps, bank + 0x610, 4), memcpy(u, bank + 0x644, 4);
    CHECK(ps == 2047.0f && u[0] == 61, "vertex c0: point size %g at 0x610, base instance %u at 0x644", ps, u[0]);
    memcpy(&a, bank + 0x120, 8);
    CHECK(a == 0x12340000 && !bank[0x200], "vertex c0: storage slot 1 at 0x120 is %#llx", (unsigned long long)a);
    return 0;
}

// ---- pitch part: a pitch texture the GPU writes holds the texels in its storage once the CPU has waited for the
// fence, as the pool memory does on the console. The shape is that of MKW's animation textures (RGBA16F 8x1218, 0x80
// bytes a row, 0x26100 bytes: over the small-target limit), which its CPU code reads in place.
#define PT_W 8
#define PT_H 1218

static int test_pitch(const GfxBackend *b, uint8_t *guest, int *bytes) {
    pool_t pool = {.magic = NVN_POOL_MAGIC, .flags = 0x22, .mem = guest + 0x800000, .size = 0x100000,
                   .game_mem = guest + 0x800000};
    CHECK(b->pool_init(&pool), "pool_init of a CPU-visible pool for the pitch part");
    uint8_t *m = pool.mem;
    static tex_t t;   // the payload's texture pointer lives as long as the submission
    init_tex(&t, &pool, 0x40000, NVN_FORMAT_RGBA16F);
    t.d.width = PT_W, t.d.height = PT_H, t.d.flags = NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET;
    nvn_tex_layout(&t.d, &t.L);
    t.serial = 301;
    uint32_t stride = nvn_tex_linear_stride(&t.d);
    CHECK(stride == 0x80 && t.L.size == (uint64_t)stride * PT_H && t.L.size > (64u << 10) && t.L.size <= 0x40000,
          "pitch texture layout: %#x bytes a row, %#llx bytes", stride, (unsigned long long)t.L.size);
    uint16_t *src = (uint16_t *)m;   // the texels as a tight buffer, 64 bytes a row
    for (int i = 0; i < PT_W * PT_H * 4; i++) src[i] = (uint16_t)(i * 31 + 7);
    memset(t.mem, 0, t.L.size);

    static _Alignas(8) uint8_t stream[512];
    uint8_t *p = put(stream, OP_BEGIN, NULL, 0);
    p_copy_b2t up;
    memset(&up, 0, sizeof up);
    up.src = (uint64_t)(uintptr_t)m, up.tex = &t, up.r = (NVNcopyRegion){0, 0, 0, PT_W, PT_H, 1};
    p = put(p, OP_COPY_B2T, &up, sizeof up);
    p = put(p, OP_END, NULL, 0);
    CHECK(p <= stream + sizeof stream, "pitch op stream overflow");
    uint64_t handle = (uint64_t)(uintptr_t)stream;
    uint64_t f = b->submit(&handle, 1);
    CHECK(b->fence_wait(f, 5000000000ull), "fence_wait of the pitch submission timed out");
    b->cpu_synced(f);   // as nvnSyncWait after its wait
    int y = 0;
    while (y < PT_H && !memcmp(t.mem + (size_t)y * stride, src + (size_t)y * PT_W * 4, PT_W * 8)) y++;
    uint16_t got = y < PT_H ? *(const uint16_t *)(t.mem + (size_t)y * stride) : 0;
    b->texture_release(&t);   // before the check: a failed probe leaves nothing allocated for the shutdown
    b->pool_fini(&pool);
    CHECK(y == PT_H, "row %d of the pitch texture's storage does not hold what the GPU copied into it (first word %#x, "
          "expected %#x)", y, got, src[(size_t)y * PT_W * 4]);
    *bytes = PT_W * PT_H * 8;
    return 0;
}

// A texture in a physical pool's memory that nothing wrote reads zero, as Switch pool memory starts zeroed
// (MKW_FIX_ZERO; the driver may return zeros without it, then this is a guard).
static int test_zero(const GfxBackend *b, uint8_t *guest, int *bytes) {
    pool_t phys = {.magic = NVN_POOL_MAGIC, .flags = 0x189, .mem = (uint8_t *)(uintptr_t)RT_GPU_VA_BASE, .size = 0x100000,
                   .game_mem = guest + 0xA00000};
    pool_t cpu = {.magic = NVN_POOL_MAGIC, .flags = 0x22, .mem = guest + 0x800000, .size = 0x100000, .game_mem = guest + 0x800000};
    CHECK(b->pool_init(&phys), "pool_init of a physical pool");
    CHECK(b->pool_init(&cpu), "pool_init of a CPU-visible pool for the zero part");
    static tex_t t;
    init_tex(&t, &phys, 0, NVN_FORMAT_RGBA8);
    t.d.width = 64, t.d.height = 64;
    nvn_tex_layout(&t.d, &t.L);
    t.serial = 301;
    memset(cpu.mem, 0xab, 64 * 64 * 4);
    static _Alignas(8) uint8_t stream[512];
    uint8_t *p = put(stream, OP_BEGIN, NULL, 0);
    p_copy_b2t down;
    memset(&down, 0, sizeof down);
    down.src = (uint64_t)(uintptr_t)cpu.mem, down.tex = &t, down.r = (NVNcopyRegion){0, 0, 0, 64, 64, 1};
    p = put(p, OP_COPY_T2B, &down, sizeof down);
    CHECK(xf_submit(b, stream, p), "fence_wait of the zero submission timed out");
    for (int i = 0; i < 64 * 64 * 4; i++) CHECK(!cpu.mem[i], "unwritten texel byte %d of a physical-pool texture is %02x", i, cpu.mem[i]);
    b->texture_release(&t);
    b->pool_fini(&cpu);
    b->pool_fini(&phys);
    *bytes = 64 * 64 * 4;
    return 0;
}

// ClearBuffer / ReportValue (OP_CLEAR_BUF), Downsample (a linear OP_COPY_T2T to half size) and the stencil view
// aspect. Fills: 64 bytes in a CPU-visible pool, a 4-byte report over the first word in the same submission (GPU order:
// the report wins), and 64 bytes in a CPU-invisible physical pool copied out with COPY_B2B. Downsample: a 64x64 RGBA8
// texture to 32x32, each texel the 2x2 average within 1. Stencil: views of a D24S8 texture pick the stencil aspect in
// STENCIL mode and depth otherwise, and a stencil-aspect sampled view of the image is created.
static int test_nvnx(const GfxBackend *b, uint8_t *guest, int *bytes) {
    pool_t cpu = {.magic = NVN_POOL_MAGIC, .flags = 0x22, .mem = guest + 0x800000, .size = 0x100000, .game_mem = guest + 0x800000};
    pool_t phys = {.magic = NVN_POOL_MAGIC, .flags = 0x189, .mem = (uint8_t *)(uintptr_t)RT_GPU_VA_BASE, .size = 0x100000,
                   .game_mem = guest + 0xA00000};
    CHECK(b->pool_init(&cpu) && b->pool_init(&phys), "pool_init of the nvnx pools");
    uint8_t *m = cpu.mem;
    memset(m, 0x11, 0x400);
    static tex_t src, dst;
    init_tex(&src, &cpu, 0x40000, NVN_FORMAT_RGBA8);
    src.d.width = 64, src.d.height = 64, src.serial = 401;
    nvn_tex_layout(&src.d, &src.L);
    init_tex(&dst, &cpu, 0x60000, NVN_FORMAT_RGBA8);
    dst.d.width = 32, dst.d.height = 32, dst.serial = 402;
    nvn_tex_layout(&dst.d, &dst.L);
    CHECK(src.L.size <= 0x20000 && dst.L.size <= 0x20000, "downsample textures exceed their pool slots");
    uint8_t *img = m + 0x1000, *out = m + 0x8000;
    for (int i = 0; i < 64 * 64 * 4; i++) img[i] = (uint8_t)((i * 37 + (i >> 8) * 11) & 0xff);
    memset(out, 0, 32 * 32 * 4);

    static _Alignas(8) uint8_t stream[2048];
    uint8_t *p = put(stream, OP_BEGIN, NULL, 0);
    const p_clear_buf f1 = {(uint64_t)(uintptr_t)m, 64, 0xdeadbeefu, 0}, rv = {(uint64_t)(uintptr_t)m, 4, 0x1234u, 0};
    p = put(p, OP_CLEAR_BUF, &f1, sizeof f1);
    p = put(p, OP_CLEAR_BUF, &rv, sizeof rv);
    const p_clear_buf f2 = {RT_GPU_VA_BASE + 0x100, 64, 0xa5a5a5a5u, 0};
    p = put(p, OP_CLEAR_BUF, &f2, sizeof f2);
    const p_copy_b2b back = {RT_GPU_VA_BASE + 0x100, (uint64_t)(uintptr_t)(m + 0x200), 64};
    p = put(p, OP_COPY_B2B, &back, sizeof back);
    p_copy_b2t up;
    memset(&up, 0, sizeof up);
    up.src = (uint64_t)(uintptr_t)img, up.tex = &src, up.r = (NVNcopyRegion){0, 0, 0, 64, 64, 1};
    p = put(p, OP_COPY_B2T, &up, sizeof up);
    p_copy_t2t ds;   // as nvnCommandBufferDownsample records it
    memset(&ds, 0, sizeof ds);
    ds.src = &src, ds.dst = &dst, ds.sr = (NVNcopyRegion){0, 0, 0, 64, 64, 1}, ds.dr = (NVNcopyRegion){0, 0, 0, 32, 32, 1};
    ds.flags = NVN_COPY_FLAGS_LINEAR_FILTER_BIT;
    p = put(p, OP_COPY_T2T, &ds, sizeof ds);
    p_copy_b2t down;
    memset(&down, 0, sizeof down);
    down.src = (uint64_t)(uintptr_t)out, down.tex = &dst, down.r = (NVNcopyRegion){0, 0, 0, 32, 32, 1};
    p = put(p, OP_COPY_T2B, &down, sizeof down);
    CHECK(xf_submit(b, stream, p), "fence_wait of the nvnx submission timed out");

    uint32_t w0;
    memcpy(&w0, m, 4);
    CHECK(w0 == 0x1234u, "ReportValue word after the fill is %#x", w0);
    for (int i = 1; i < 16; i++) {
        uint32_t w;
        memcpy(&w, m + 4 * i, 4);
        CHECK(w == 0xdeadbeefu, "ClearBuffer word %d is %#x", i, w);
    }
    CHECK(m[64] == 0x11, "ClearBuffer wrote past its 64 bytes (%02x)", m[64]);
    for (int i = 0; i < 64; i++) CHECK(m[0x200 + i] == 0xa5, "physical-pool fill byte %d is %02x", i, m[0x200 + i]);
    int worst = 0;
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++)
            for (int ch = 0; ch < 4; ch++) {
                int sum = 0;
                for (int dy = 0; dy < 2; dy++)
                    for (int dx = 0; dx < 2; dx++) sum += img[((2 * y + dy) * 64 + 2 * x + dx) * 4 + ch];
                int d = abs(out[(y * 32 + x) * 4 + ch] - (sum + 2) / 4);
                if (d > worst) worst = d;
            }
    CHECK(worst <= 1, "downsampled texel off the 2x2 average by %d", worst);

    static tex_t dsx;
    init_tex(&dsx, &cpu, 0x80000, NVN_FORMAT_DEPTH24_STENCIL8);
    dsx.d.width = 64, dsx.d.height = 64, dsx.serial = 403;
    nvn_tex_layout(&dsx.d, &dsx.L);
    const VkTexImage *di = vk_texture_get(&dsx);
    CHECK(di && (di->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) && (di->aspect & VK_IMAGE_ASPECT_DEPTH_BIT), "D24S8 image with both aspects");
    view_t sv, dv;
    memset(&sv, 0, sizeof sv), memset(&dv, 0, sizeof dv);
    sv.set_dsmode = 1, sv.dsmode = NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL;
    dv.set_dsmode = 1, dv.dsmode = NVN_TEXTURE_DEPTH_STENCIL_MODE_DEPTH;
    CHECK(vk_exec_view_aspect(di, &sv, 1) == VK_IMAGE_ASPECT_STENCIL_BIT && vk_exec_view_aspect(di, &dv, 1) == VK_IMAGE_ASPECT_DEPTH_BIT &&
              vk_exec_view_aspect(di, &sv, 0) == VK_IMAGE_ASPECT_DEPTH_BIT, "view aspects of a D24S8 texture");
    VkImageViewCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = di->image,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = di->format,
                                .subresourceRange = {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1}};
    VkImageView view = VK_NULL_HANDLE;
    CHECK(vkCreateImageView(vk_ctx()->device, &ci, NULL, &view) == VK_SUCCESS, "stencil-aspect view of the D24S8 image");
    vkDestroyImageView(vk_ctx()->device, view, NULL);
    b->texture_release(&src);
    b->texture_release(&dst);
    b->texture_release(&dsx);
    b->pool_fini(&cpu);
    b->pool_fini(&phys);
    *bytes = 64 + 64 + 32 * 32 * 4;
    return 0;
}

// VRAM mirror (vk_memory.c; skipped in import mode): a pool given its mirror as on a first buffer use, after which
// buffer copies and fills read and write the mirror. The first use copies the whole pool; CPU writes between
// submissions reach the mirror through write watch; GPU writes reach guest memory by the fence, and only the bytes
// written: CPU bytes beside a GPU fill keep their values.
static int test_mirror(const GfxBackend *b, uint8_t *guest, int *bytes) {
    *bytes = 0;
    if (!vk_mirror_on) return 0;
    pool_t pool = {.magic = NVN_POOL_MAGIC, .flags = 0x22, .mem = guest + 0xC00000, .size = 0x100000,
                   .game_mem = guest + 0xC00000};
    CHECK(b->pool_init(&pool), "pool_init of a CPU-visible pool for the mirror part");
    uint8_t *m = pool.mem;
    for (int i = 0; i < 4096; i++) m[i] = (uint8_t)(i * 13 + 1);
    memset(m + 0x1000, 0xEE, 0x5000);
    CHECK(vk_mem_mirror_probe((uint64_t)(uintptr_t)m), "the pool got no VRAM mirror");
    static _Alignas(8) uint8_t stream[1024];
    uint64_t a = (uint64_t)(uintptr_t)m;
    // 1: B2B in the mirror after its first-use copy, and a fill; both written back.
    uint8_t *p = put(stream, OP_BEGIN, NULL, 0);
    const p_copy_b2b c1 = {a, a + 0x1000, 4096};
    const p_clear_buf f1 = {a + 0x2000, 0x800, 0x5a5a5a5au, 0};
    p = put(p, OP_COPY_B2B, &c1, sizeof c1);
    p = put(p, OP_CLEAR_BUF, &f1, sizeof f1);
    CHECK(xf_submit(b, stream, p), "fence_wait of the first mirror submission timed out");
    for (int i = 0; i < 4096; i++) CHECK(m[0x1000 + i] == m[i], "mirror B2B byte %d is %02x, expected %02x", i, m[0x1000 + i], m[i]);
    for (int i = 0; i < 0x800; i++) CHECK(m[0x2000 + i] == 0x5a, "mirror fill byte %d is %02x", i, m[0x2000 + i]);
    for (int i = 0x800; i < 0x1000; i++) CHECK(m[0x2000 + i] == 0xEE, "byte %#x past the mirror fill is %02x", i, m[0x2000 + i]);
    // 2: the CPU rewrites the source; the next submission copies the new bytes.
    for (int i = 0; i < 4096; i++) m[i] = (uint8_t)(i * 7 + 5);
    p = put(stream, OP_BEGIN, NULL, 0);
    const p_copy_b2b c2 = {a, a + 0x3000, 4096};
    p = put(p, OP_COPY_B2B, &c2, sizeof c2);
    CHECK(xf_submit(b, stream, p), "fence_wait of the second mirror submission timed out");
    for (int i = 0; i < 4096; i++)
        CHECK(m[0x3000 + i] == m[i], "B2B byte %d after a CPU write is %02x, expected %02x (write not mirrored)", i,
              m[0x3000 + i], m[i]);
    // 3: CPU bytes in the page of a GPU fill, written before the submission, survive the write-back.
    memset(m + 0x4000, 0x11, 0x800);
    p = put(stream, OP_BEGIN, NULL, 0);
    const p_clear_buf f3 = {a + 0x4800, 0x800, 0x77777777u, 0};
    p = put(p, OP_CLEAR_BUF, &f3, sizeof f3);
    CHECK(xf_submit(b, stream, p), "fence_wait of the third mirror submission timed out");
    for (int i = 0; i < 0x800; i++) CHECK(m[0x4000 + i] == 0x11, "CPU byte %#x beside a fill is %02x", i, m[0x4000 + i]);
    for (int i = 0; i < 0x800; i++) CHECK(m[0x4800 + i] == 0x77, "fill byte %#x is %02x", i, m[0x4800 + i]);
    b->pool_fini(&pool);
    *bytes = 4096 + 0x1000 + 4096 + 0x1000;
    return 0;
}

// MKW_RTSTATS (diag/vk_rtstats.c), on for this part only, every frame: an RGBA16F target cleared to grey with a NaN
// rectangle cleared in a pass breaks the nan rule and not the constant one; the same target cleared twice to grey
// breaks the constant one. Each rule is counted from what the part adds.
static int test_rtstats(const GfxBackend *b, uint8_t *guest) {
    pool_t pool = {.magic = NVN_POOL_MAGIC, .flags = 0x22, .mem = guest + 0xE00000, .size = 0x100000,
                   .game_mem = guest + 0xE00000};
    CHECK(b->pool_init(&pool), "pool_init of a CPU-visible pool for the rtstats part");
    static tex_t t;
    init_tex(&t, &pool, 0, NVN_FORMAT_RGBA16F);
    t.serial = 501;
    CHECK(t.L.size <= 0x100000, "rtstats texture exceeds its pool");
    vk_rtstats_every(1);
    uint64_t nan0 = vk_rtstats_hits(VK_RTSTATS_NAN), inf0 = vk_rtstats_hits(VK_RTSTATS_INF);
    uint64_t const0 = vk_rtstats_hits(VK_RTSTATS_CONST);
    for (int k = 0; k < 2; k++) {   // 0: NaN rectangle, 1: grey everywhere
        static _Alignas(8) uint8_t stream[1024];
        uint8_t *p = put(stream, OP_BEGIN, NULL, 0);
        p_set_rt rt;
        memset(&rt, 0, sizeof rt);
        rt.n = 1, rt.color[0] = &t;
        p = put(p, OP_SET_RT, &rt, sizeof rt);
        const p_clear_color grey = {0, 0xf, {0.5f, 0.5f, 0.5f, 1.0f}}, nan = {0, 0xf, {NAN, NAN, NAN, NAN}};
        p = put(p, OP_CLEAR_COLOR, &grey, sizeof grey);   // outside a pass: not checked
        const p_scissor sc = {SC_X, SC_Y, SC_W, SC_H}, full = {0, 0, 1 << 20, 1 << 20};
        p = put(p, OP_SCISSOR, &sc, sizeof sc);
        p = put(p, OP_CLEAR_COLOR, k ? &grey : &nan, sizeof grey);   // in a pass, checked at its end
        p = put(p, OP_SCISSOR, &full, sizeof full);
        CHECK(xf_submit(b, stream, p), "fence_wait of rtstats submission %d timed out", k);
        vk_rtstats_flush();
        uint64_t dn = vk_rtstats_hits(VK_RTSTATS_NAN) - nan0, dc = vk_rtstats_hits(VK_RTSTATS_CONST) - const0;
        CHECK(dn == 1 && dc == (uint64_t)k, "after submission %d: nan rule hit %llu times, constant %llu (expected 1, %d)",
              k, (unsigned long long)dn, (unsigned long long)dc, k);
    }
    CHECK(vk_rtstats_hits(VK_RTSTATS_INF) == inf0, "inf rule hit on targets without an infinity");
    b->texture_release(&t);
    b->pool_fini(&pool);
    return 0;
}


// A sparse virtual pool starts without mapped texels. Its privately backed small mips must not expose an old image's bytes.
static int test_sparse_tail_zero(const GfxBackend *b) {
    static uint8_t pixels[64 * 64 * 2 * 4];
    for (unsigned round = 0; round < 12; round++) {
        pool_t pool = {.magic = NVN_POOL_MAGIC,
                      .flags = NVN_MEMORY_POOL_FLAGS_VIRTUAL | NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_GPU_CACHED,
                      .mem = (uint8_t *)(uintptr_t)(RT_GPU_VA_BASE + 0x10000000ull + round * 0x1000000ull), .size = 0x1000000};
        CHECK(b->pool_init(&pool), "sparse tail virtual pool init");
        tex_t t;
        init_tex(&t, &pool, 0, NVN_FORMAT_RGBA8);
        t.d.target = NVN_TEXTURE_TARGET_2D_ARRAY;
        t.d.width = t.d.height = 512, t.d.depth = 2, t.d.levels = 10;
        t.d.flags = NVN_TEXTURE_FLAGS_SPARSE;
        nvn_tex_layout(&t.d, &t.L);
        t.serial = 10000 + round;
        const VkTexImage *image = vk_texture_get(&t);
        CHECK(image, "sparse tail probe did not create an image");
        if (!vk_sparse_is(image->image)) {
            b->texture_release(&t); b->pool_fini(&pool);
            puts("probe-vkexec: sparse tail check skipped (sparse images unavailable)");
            return 0;
        }
        view_t v = {.set_levels = 1, .base_level = 3, .nlevels = 1};
        NVNcopyRegion region = {0, 0, 0, 64, 64, 2};
        memset(pixels, 0xcd, sizeof pixels);
        b->read_texels(&t, &v, &region, pixels, 64 * 4, 64 * 64 * 4);
        int first = -1; unsigned byte = 0;
        for (unsigned i = 0; i < sizeof pixels; i++) if (pixels[i]) {first = (int)i; byte = pixels[i]; break;}
        // Poison the tail before releasing it: a later same-size allocation can reuse this device memory.
        memset(pixels, 0xa5, sizeof pixels);
        b->write_texels(&t, &v, &region, pixels, 64 * 4, 64 * 64 * 4);
        b->finish();
        b->read_texels(&t, &v, &region, pixels, 64 * 4, 64 * 64 * 4);
        int changed = -1;
        for (unsigned i = 0; i < sizeof pixels; i++) if (pixels[i] != 0xa5) {changed = (int)i; break;}
        b->texture_release(&t); b->pool_fini(&pool);
        for (int k = 0; k < 3; k++) b->submit(NULL, 0);
        b->finish();
        CHECK(first < 0, "unwritten sparse tail round %u byte %d is %#x, expected zero", round, first, byte);
        CHECK(changed < 0, "written sparse tail round %u byte %d was lost", round, changed);
    }
    puts("probe-vkexec: sparse tails start at zero across dirty memory reuse");
    return 0;
}

int probe_vkexec(void) {
    main_tid = plat_thread_id();
    // The exact colour checks need the plain picture, whatever mkw.ini says: the SDR swapchain (HDR10, MKW_HDR auto on an
    // HDR screen, encodes them in PQ) and no performance overlay (it draws over the top right corner)
    plat_setenv("MKW_HDR", "off");
    plat_setenv("MKW_PERF_OVERLAY", "off");
    const GfxBackend *b = &gfx_vulkan_backend;
    uint64_t heap = 0;
    CHECK(mem_set_heap_size(16ull << 20, &heap) == 0 && heap, "SetHeapSize 16 MiB");
    uint8_t *guest = (uint8_t *)(uintptr_t)heap;
    uint64_t first = 0, last = 0;
    int w = 0, h = 0, inside = 0, xfer_bytes = 0;
    int bad = test_c0();   // CPU only: before the device checks
    if (!bad) bad = test_recent();
    if (!bad) bad = test_sync(b, guest, &first, &last) || test_present(b, guest, &w, &h);
    int sync_bad = bad;
    if (!bad) bad = test_clear(b, guest, &inside);
    if (!bad) bad = test_xfer(b, guest, &xfer_bytes);
    int pitch_bytes = 0;
    if (!bad) bad = test_pitch(b, guest, &pitch_bytes);
    int zero_bytes = 0;
    if (!bad) bad = test_zero(b, guest, &zero_bytes);
    if (!bad) bad = test_sparse_tail_zero(b);
    int nvnx_bytes = 0, mirror_bytes = 0;
    if (!bad) bad = test_nvnx(b, guest, &nvnx_bytes);
    if (!bad) bad = test_mirror(b, guest, &mirror_bytes);
    if (!bad) bad = test_rtstats(b, guest);
    b->finish();
    vk_pipeline_shutdown();   // the pipelines and the replay's stand-ins go before the device
    vk_exec_draw_shutdown();
    vk_exec_xfer_shutdown();
    vk_backend_shutdown();
    long errors = vk_validation_errors_total();
    if (bad || errors) {
        printf("probe-vkexec: FAILED (%s, %ld validation errors)\n",
               sync_bad ? "a sync check failed" : bad ? "a clear or xfer check failed" : "checks passed", errors);
        return 1;
    }
    printf("probe-vkexec: sync part passed (%d submissions, fences %llu..%llu, %d completion callbacks in submit order "
           "on the completion thread, guest word visible after fence_wait, finish drained, present %dx%d exact, "
           "0 validation errors)\n",
           SUBMITS + TAIL + 1, (unsigned long long)first, (unsigned long long)last, SUBMITS + TAIL, w, h);
    printf("probe-vkexec: clear part passed (RGBA8 + D32 %dx%d: image clears outside a pass, scissored attachment "
           "clears in a pass on %d texels, read back exact, 0 validation errors)\n",
           TEX_W, TEX_H, inside);
    printf("probe-vkexec: xfer part passed (B2B between CPU-visible pools, B2T + T2B round trip and T2T of a %dx%d RGBA8 "
           "texture exact, CLEAR_TEX exact, event WRITE + ADD, timestamp and samples-passed reports in guest memory after "
           "fence_wait, %d bytes compared, 0 validation errors)\n",
           XF_W, XF_H, xfer_bytes);
    printf("probe-vkexec: pitch part passed (a %dx%d RGBA16F LINEAR_RENDER_TARGET texture filled by a buffer copy holds the "
           "texels in its pool bytes after the fence wait, %d bytes compared)\n", PT_W, PT_H, pitch_bytes);
    printf("probe-vkexec: nvnx part passed (ClearBuffer then ReportValue in GPU order, a CPU-invisible pool fill copied out, "
           "Downsample 64x64 -> 32x32 within 1 of the 2x2 average, stencil view aspect of D24S8, %d bytes compared)\n",
           nvnx_bytes);
    printf("probe-vkexec: c0 and zero parts passed (c0 tables, dispatch size, point size and base instance at their offsets, "
           "unbound slots zero; %d bytes of an unwritten physical-pool texture read zero, MKW_FIX_ZERO=%s)\n",
           zero_bytes, cfg_value("MKW_FIX_ZERO"));
    printf("probe-vkexec: rtstats part passed (MKW_RTSTATS: a NaN rectangle in a %dx%d RGBA16F target hit the nan rule "
           "only, the same target all grey the constant rule only, no inf)\n", TEX_W, TEX_H);
    printf("probe-vkexec: target history part passed (a target rendered the frame before and a layer rendered 12 frames "
           "before are recent; one last rendered 60 frames before and one never rendered are not)\n");
    if (vk_mirror_on)
        printf("probe-vkexec: mirror part passed (B2B and fill in a VRAM mirror written back by the fence, a CPU write "
               "between submissions mirrored, CPU bytes beside a fill kept, %d bytes compared)\n", mirror_bytes);
    else
        printf("probe-vkexec: mirror part skipped (memory mode import)\n");
    return 0;
}
