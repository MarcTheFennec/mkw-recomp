// Integer-format images of a compute program (gfx_shader.c int_image_variant). The translator declares every texture
// and storage image with a float sampled type and moves raw bits through it (floatBitsToUint after a read,
// uintBitsToFloat before a write), as SM86's TLD/TLD4/SULD/SUST do; the bound view's format decides what the bits
// are. In Vulkan a float image over a UINT/SINT view is undefined (VUID format-07753): AMD and NVIDIA return the
// bits, Intel Arc drew the race black (issue #64). spirv_int_images retypes the variables of the bindings in umask
// (uint) and smask (int) to images of that type, bitcasts every read result back to the float vector the shader
// expects and every written texel to the integer vector. eden-nx2 types them at translation from the bound
// descriptor (idea only, GPL-3.0: texture_pass.cpp is_integer, emit_spirv_image.cpp's bitcasts).

#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#include "host.h"

enum { OP_TYPE_INT = 21, OP_TYPE_FLOAT = 22, OP_TYPE_VECTOR = 23, OP_TYPE_IMAGE = 25, OP_TYPE_SAMPLED_IMAGE = 27,
       OP_TYPE_POINTER = 32, OP_FUNCTION = 54, OP_VARIABLE = 59, OP_LOAD = 61, OP_DECORATE = 71,
       OP_SAMPLED_IMAGE = 86, OP_SAMPLE_IMPLICIT = 87, OP_SAMPLE_EXPLICIT = 88, OP_FETCH = 95, OP_GATHER = 96,
       OP_READ = 98, OP_WRITE = 99, OP_IMAGE = 100, OP_BITCAST = 124, DEC_BINDING = 33, SC_UNIFORM_CONSTANT = 0 };

#define MAX_VARS 64

// An image type equal to w[at]'s but for its sampled type st: one the module or ts[0..nt) declares, else 0 (SPIR-V
// allows no two equal image types).
static uint32_t same_image(const uint32_t *w, uint32_t bound, const uint64_t *img_at, uint64_t at, uint32_t st,
                           const uint32_t (*ts)[2], int nt, const uint64_t *made_at) {
    uint32_t len = w[at] >> 16;
    for (uint32_t id = 1; id < bound; id++) {
        const uint32_t *t = img_at[id] ? w + img_at[id] : NULL;
        if (t && t[0] == w[at] && t[2] == st && !memcmp(t + 3, w + at + 3, (len - 3) * 4)) return id;
    }
    for (int i = 0; i < nt; i++)
        if (ts[i][1] == st && made_at[i] == at) return ts[i][0];
    return 0;
}

uint32_t *spirv_int_images(const uint32_t *w, uint64_t n, uint64_t umask, uint64_t smask, uint64_t *out_n,
                           const char **why) {
    if (why) *why = NULL;
    if (out_n) *out_n = 0;
    if (!w || !out_n || n < 5 || w[0] != 0x07230203 || !w[3]) {
        if (why) *why = "invalid SPIR-V header";
        return NULL;
    }
    const char *error = "malformed SPIR-V instruction";
    uint32_t bound = w[3], f32 = 0, i32 = 0, u32 = 0, v4f = 0, v4i = 0, v4u = 0;
    uint32_t *binding = calloc(bound, sizeof *binding);   // Binding + 1 of each id
    uint32_t *pointee = calloc(bound, sizeof *pointee);   // UniformConstant pointer type -> pointee
    uint32_t *si_img = calloc(bound, sizeof *si_img);     // sampled image type -> its image type
    uint64_t *img_at = calloc(bound, sizeof *img_at);     // image type -> word index of its OpTypeImage
    uint32_t *newt = calloc(bound, sizeof *newt);         // retyped value id -> its new (sampled) image type
    uint8_t *kind = calloc(bound, 1);                      // retyped value id: 1 uint, 2 int
    uint32_t *out = malloc(sizeof *out * (size_t)(n * 2 + 64 + 16 * MAX_VARS));
    if (!binding || !pointee || !si_img || !img_at || !newt || !kind || !out) rt_fatal("gfx_shader: out of memory (int images)");
    uint64_t fn_at = n;
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n) goto fail;
        const uint32_t *a = w + i + 1;
        if (op == OP_TYPE_FLOAT && len >= 3 && a[1] == 32) f32 = a[0];
        else if (op == OP_TYPE_INT && len >= 4 && a[1] == 32) { if (a[2]) i32 = a[0]; else u32 = a[0]; }
        else if (op == OP_DECORATE && len >= 4 && a[1] == DEC_BINDING && a[0] < bound) binding[a[0]] = a[2] + 1;
        else if (op == OP_TYPE_POINTER && len == 4 && a[1] == SC_UNIFORM_CONSTANT && a[0] < bound) pointee[a[0]] = a[2];
        else if (op == OP_TYPE_SAMPLED_IMAGE && len == 3 && a[0] < bound) si_img[a[0]] = a[1];
        else if (op == OP_TYPE_IMAGE && len >= 9 && a[0] < bound) img_at[a[0]] = i;
        else if (op == OP_FUNCTION && fn_at == n) fn_at = i;
        i += len;
    }
    // Some compute programs already declare integer storage images (for image atomics), and need no float vec4 at
    // all. A correct binding is a successful no-op, not a failed conversion. Check before requiring float types.
    int matched = 0, needs_work = 0;
    for (uint64_t i = 5; i < fn_at;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        const uint32_t *a = w + i + 1;
        if (op == OP_VARIABLE && len == 4 && a[2] == SC_UNIFORM_CONSTANT && a[1] < bound && a[0] < bound &&
            binding[a[1]] && binding[a[1]] <= 64) {
            uint32_t b = binding[a[1]] - 1, t = pointee[a[0]], im = t < bound && si_img[t] ? si_img[t] : t;
            uint64_t bit = 1ull << b;
            if (((umask | smask) & bit) && im < bound && img_at[im]) {
                matched = 1;
                uint32_t want = umask & bit ? u32 : i32;
                if (!want || w[img_at[im] + 2] != want) needs_work = 1;
            }
        }
        i += len;
    }
    if (matched && !needs_work) {
        memcpy(out, w, (size_t)n * 4);
        free(binding), free(pointee), free(si_img), free(img_at), free(newt), free(kind);
        *out_n = n;
        return out;
    }
    for (uint64_t i = 5; i < fn_at;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        const uint32_t *a = w + i + 1;
        if (op == OP_TYPE_VECTOR && len >= 4 && a[2] == 4) {
            if (a[1] == f32) v4f = a[0];
            else if (a[1] == i32) v4i = a[0];
            else if (a[1] == u32) v4u = a[0];
        }
        i += len;
    }
    if (!f32 || !v4f) { error = "no float32 vec4 type"; goto fail; }
    uint32_t nu = bound;
    if (!u32) u32 = nu++;
    if (!i32) i32 = nu++;
    if (!v4u) v4u = nu++;
    if (!v4i) v4i = nu++;
    uint64_t o = 0;
    for (uint64_t i = 0; i < 5; i++) out[o++] = w[i];
    // The global section as it was, minus the retyped variables; their new image, sampled image and pointer types and
    // the variables themselves go at its end, before the first function, where every type they name is declared.
    struct { uint32_t var, k, ptr, img, si, combined, old_img, new_img, new_si; } mv[MAX_VARS];
    uint32_t made[MAX_VARS][2];   // image types made: id, sampled type
    uint64_t made_at[MAX_VARS];   // the OpTypeImage each was copied from
    int nmv = 0, nmade = 0;
    for (uint64_t i = 5; i < fn_at;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        const uint32_t *a = w + i + 1;
        if (op == OP_VARIABLE && len == 4 && a[2] == SC_UNIFORM_CONSTANT && a[1] < bound && a[0] < bound &&
            binding[a[1]] && binding[a[1]] <= 64 && nmv < MAX_VARS) {
            uint32_t b = binding[a[1]] - 1, k = umask >> b & 1 ? 1 : smask >> b & 1 ? 2 : 0;
            uint32_t t = pointee[a[0]], si = t < bound && si_img[t] ? t : 0, im = si ? si_img[t] : t;
            if (k && im < bound && img_at[im] && w[img_at[im] + 2] == f32) {
                uint32_t st = k == 1 ? u32 : i32;
                mv[nmv].var = a[1], mv[nmv].k = k, mv[nmv].old_img = im, mv[nmv].new_img = mv[nmv].new_si = 0;
                mv[nmv].combined = si != 0;
                mv[nmv].img = same_image(w, bound, img_at, img_at[im], st, (const uint32_t (*)[2])made, nmade, made_at);
                if (!mv[nmv].img) {
                    mv[nmv].img = mv[nmv].new_img = nu++;
                    made[nmade][0] = mv[nmv].img, made[nmade][1] = st, made_at[nmade++] = img_at[im];
                }
                mv[nmv].si = 0;
                // Separate texture/sampler descriptors construct OpSampledImage in the function body. Declare its
                // integer sampled-image type here too; storage images (Sampled = 2) must not get such a type.
                if (si || w[img_at[im] + 7] == 1) {
                    for (uint32_t id = 1; id < bound && !mv[nmv].si; id++)
                        if (si_img[id] == mv[nmv].img) mv[nmv].si = id;
                    for (int m = 0; m < nmv && !mv[nmv].si; m++)
                        if (mv[m].new_si && mv[m].img == mv[nmv].img) mv[nmv].si = mv[m].new_si;
                    if (!mv[nmv].si) mv[nmv].si = mv[nmv].new_si = nu++;
                }
                mv[nmv].ptr = nu++;
                kind[a[1]] = (uint8_t)k, newt[a[1]] = si ? mv[nmv].si : mv[nmv].img;
                nmv++;
                i += len;
                continue;
            }
        }
        for (uint32_t j = 0; j < len; j++) out[o++] = w[i + j];
        i += len;
    }
    if (!nmv) { error = "no matching float image binding"; goto fail; }
    if (u32 >= bound) out[o++] = 4u << 16 | OP_TYPE_INT, out[o++] = u32, out[o++] = 32, out[o++] = 0;
    if (i32 >= bound) out[o++] = 4u << 16 | OP_TYPE_INT, out[o++] = i32, out[o++] = 32, out[o++] = 1;
    if (v4u >= bound) out[o++] = 4u << 16 | OP_TYPE_VECTOR, out[o++] = v4u, out[o++] = u32, out[o++] = 4;
    if (v4i >= bound) out[o++] = 4u << 16 | OP_TYPE_VECTOR, out[o++] = v4i, out[o++] = i32, out[o++] = 4;
    for (int m = 0; m < nmv; m++) {
        const uint32_t *ti = w + img_at[mv[m].old_img];
        uint32_t len = ti[0] >> 16;
        if (mv[m].new_img) {
            out[o++] = ti[0], out[o++] = mv[m].img, out[o++] = mv[m].k == 1 ? u32 : i32;
            for (uint32_t j = 3; j < len; j++) out[o++] = ti[j];
        }
        if (mv[m].new_si) out[o++] = 3u << 16 | OP_TYPE_SAMPLED_IMAGE, out[o++] = mv[m].si, out[o++] = mv[m].img;
        out[o++] = 4u << 16 | OP_TYPE_POINTER, out[o++] = mv[m].ptr, out[o++] = SC_UNIFORM_CONSTANT;
        out[o++] = mv[m].combined ? mv[m].si : mv[m].img;
        out[o++] = 4u << 16 | OP_VARIABLE, out[o++] = mv[m].ptr, out[o++] = mv[m].var, out[o++] = SC_UNIFORM_CONSTANT;
    }
    // function bodies: loads of the variables and images taken from their sampled images carry the new types; reads
    // through them give the integer vector in a new id, bitcast to the float vector under the old one; a written
    // texel is bitcast to the integer vector first.
    for (uint64_t i = fn_at; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n) goto fail;
        const uint32_t *a = w + i + 1;
        if ((op == OP_LOAD || op == OP_IMAGE) && len >= 4 && a[2] < bound && kind[a[2]] && a[1] < bound) {
            uint32_t t = newt[a[2]];
            if (op == OP_IMAGE) {   // the image of a retyped sampled image: the image type before it
                t = 0;
                for (int m = 0; m < nmv; m++)
                    if (mv[m].si == newt[a[2]]) t = mv[m].img;
                if (!t) { error = "sampled image type not tracked"; goto fail; }
            }
            kind[a[1]] = kind[a[2]], newt[a[1]] = t;
            out[o++] = w[i], out[o++] = t;
            for (uint32_t j = 1; j < len - 1; j++) out[o++] = a[j];
            i += len;
            continue;
        }
        if ((op == OP_SAMPLE_IMPLICIT || op == OP_SAMPLE_EXPLICIT || op == OP_FETCH || op == OP_GATHER || op == OP_READ) &&
            len >= 5 && a[2] < bound && kind[a[2]]) {
            if (a[0] != v4f) { error = "image read is not float32 vec4"; goto fail; }
            uint32_t vt = kind[a[2]] == 1 ? v4u : v4i, tmp = nu++;
            out[o++] = w[i], out[o++] = vt, out[o++] = tmp;
            for (uint32_t j = 2; j < len - 1; j++) out[o++] = a[j];
            out[o++] = 4u << 16 | OP_BITCAST, out[o++] = v4f, out[o++] = a[1], out[o++] = tmp;
            i += len;
            continue;
        }
        if (op == OP_WRITE && len >= 4 && a[0] < bound && kind[a[0]]) {
            uint32_t tmp = nu++;
            out[o++] = 4u << 16 | OP_BITCAST, out[o++] = kind[a[0]] == 1 ? v4u : v4i, out[o++] = tmp, out[o++] = a[2];
            out[o++] = w[i], out[o++] = a[0], out[o++] = a[1], out[o++] = tmp;
            for (uint32_t j = 3; j < len - 1; j++) out[o++] = a[j];
            i += len;
            continue;
        }
        if (op == OP_SAMPLED_IMAGE && len == 5 && a[2] < bound && kind[a[2]] && a[1] < bound) {
            uint32_t t = 0;
            for (int m = 0; m < nmv && !t; m++)
                if (mv[m].img == newt[a[2]]) t = mv[m].si;
            if (!t) { error = "separate sampled image type not tracked"; goto fail; }
            kind[a[1]] = kind[a[2]], newt[a[1]] = t;
            out[o++] = w[i], out[o++] = t;
            for (uint32_t j = 1; j < len - 1; j++) out[o++] = a[j];
            i += len;
            continue;
        }
        for (uint32_t j = 0; j < len; j++) out[o++] = w[i + j];
        i += len;
    }
    out[3] = nu;
    free(binding), free(pointee), free(si_img), free(img_at), free(newt), free(kind);
    *out_n = o;
    return out;
fail:
    if (why) *why = error;
    free(binding), free(pointee), free(si_img), free(img_at), free(newt), free(kind), free(out);
    return NULL;
}
