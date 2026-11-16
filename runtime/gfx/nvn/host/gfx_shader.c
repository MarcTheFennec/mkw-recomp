// Shaders of MKW: SPIR-V prepared before the game runs (tools/shaders/translate.py), one <key>.spv and <key>.res.json
// per program under <exe dir>/spirv, key = nvn2_program_key(control, code). A program is loaded once and kept for the
// life of the process in a memory cache by that key, which is also the shader's id in a pipeline key record
// (vk_pipeline.c). Vertex shaders that read integer vertex formats get a variant with integer inputs
// (int_input_variant), fragment shaders that write integer targets one with integer outputs (int_output_variant).
//
// Reflection: every shader that enters the cache has its .res.json parsed into GfxShaderRes arrays and its SPIR-V
// scanned for interface Locations and the workgroup size (gfx.h accessors at the end of the file).
//
// Provenance: the c2 handle ranges of handle_slot are those of MacNativeGames' mkw_xlate Service.cs (MIT).
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "nvn_api.h"
#include "gfx.h"
#include "host.h"
#include "platform/platform.h"

#define BUCKETS 4096
#define PATH_CAP 1024                                  // bytes of a UTF-8 path

struct GfxShader {
    struct GfxShader *next;
    uint8_t key[32];
    uint8_t iid[32];                                       // the id in pipeline key records: the program key
    uint32_t *spirv;
    uint64_t words;
    char *reflection;                                      // the service's .res.json text, parsed into the fields below
    uint64_t reflection_size;
    // Reflection (reflect(), once per shader before it enters the cache)
    int stage;                                             // GFX_STAGE_*
    int mesh_kind;                                         // GFX_MESH_*: task and mesh programs count as GFX_STAGE_VS
    uint8_t cs_table[5];   // NVN2 compute c0 table: uniform, storage, texture, image counts; push constant words before
                                                           // entries (control +0xb1/+0xb2/+0xb3/+0xb6); see cs_res
    int cs_known;                                          // cs_table read from the program's control
    GfxShaderRes *res[GFX_RES_KINDS];
    int nres[GFX_RES_KINDS];
    uint32_t in_mask, out_mask;                            // user Locations of Input / Output variables
    GfxShaderIo in[32], out[32];
    uint32_t local[3];
    uint32_t *spec;                                        // "spec": word k = bank << 16 | byte (specialization ids 2k, 2k+1)
    int nspec;
    const struct GfxShader *variant_of;                    // gfx_shader_variant: the generic shader; NULL for one
    struct GfxShader *cs_variant;                          // task/mesh: the compute form (gfx_shader_cs_variant)
    int cs_of_mesh;                                        // this is the compute form of a task or mesh program
};

static PlatMutex cache_lock = PLAT_MUTEX_INIT;                  // memory cache
static GfxShader *buckets[BUCKETS];
static volatile int64_t n_keys;                            // memory cache entries

#define read_file plat_file_read

// ---- cache

static GfxShader *mem_find(const uint8_t key[32]) {
    plat_mutex_lock_shared(&cache_lock);
    GfxShader *s = buckets[(key[0] | key[1] << 8) % BUCKETS];
    while (s && memcmp(s->key, key, 32)) s = s->next;
    plat_mutex_unlock_shared(&cache_lock);
    return s;
}

static void shader_free(GfxShader *s) {
    for (int k = 0; k < GFX_RES_KINDS; k++) free(s->res[k]);
    free(s->spirv), free(s->reflection), free(s->spec), free(s);
}

static void reflect(GfxShader *s);

// Inserts s, or frees it and returns the entry another thread inserted first.
static GfxShader *mem_insert(GfxShader *s) {
    plat_mutex_lock(&cache_lock);
    GfxShader **head = &buckets[(s->key[0] | s->key[1] << 8) % BUCKETS], *r = *head;
    while (r && memcmp(r->key, s->key, 32)) r = r->next;
    if (r) {
        shader_free(s);
        s = r;
    } else {
        s->next = *head;
        *head = s;
        n_keys++;
    }
    plat_mutex_unlock(&cache_lock);
    return s;
}

static void key_hex(const uint8_t key[32], char out[65]) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) out[i * 2] = hex[key[i] >> 4], out[i * 2 + 1] = hex[key[i] & 15];
    out[64] = 0;
}

// A .res.json the translator wrote: one JSON object that names its stage, no NUL inside. A file an interrupted write
// left zero-filled (3647 of 21924 in one cache were) fails this; reflect() would take it for a vertex shader with
// no resources, and its pipeline layout would lack the shader's buffers and textures.
static int reflection_valid(const char *res, uint64_t size) {
    if (strlen(res) != size) return 0;
    const char *b = res, *e = res + size;
    while (b < e && isspace((unsigned char)*b)) b++;
    while (e > b && isspace((unsigned char)e[-1])) e--;
    if (e - b < 2 || *b != '{' || e[-1] != '}') return 0;
    const char *st = strstr(b, "\"stage\"");
    if (!st) return 0;
    st = strchr(st + 7, '"');
    return st && (!strncmp(st, "\"vs\"", 4) || !strncmp(st, "\"ps\"", 4) || !strncmp(st, "\"cs\"", 4) ||
                  !strncmp(st, "\"task\"", 6) || !strncmp(st, "\"mesh\"", 6));   // task/mesh: reflect() mesh_kind
}

// Every instruction after the header has a word count and the last one ends at the module's end: a module with
// zero-filled clusters fails.
static int spirv_complete(const uint32_t *w, uint64_t n) {
    uint64_t i = 5;
    while (i < n) {
        uint32_t len = w[i] >> 16;
        if (!len) return 0;
        i += len;
    }
    return i == n;
}

// ---- MKW: SPIR-V prepared before the game runs (tools/shaders/translate.py)
// <game dir>/spirv/<key>.spv and .res.json, key = nvn2_program_key(control, code): every program the game ships was
// translated offline, so play never translates. MKW_SPIRV names another directory.
static char prepared_dir[PATH_CAP];
_Static_assert(CFG_GAME_DIR_MAX + 256 <= PATH_CAP, "game directory must leave room for derived shader paths");

static void prepared_init(void *param) {
    (void)param;
    const char *e = cfg_get("MKW_SPIRV");
    if (e && *e) {
        snprintf(prepared_dir, sizeof prepared_dir, "%s", e);
    } else {
        snprintf(prepared_dir, sizeof prepared_dir, "%s/spirv", cfg_game_dir());
    }
    rt_log("gfx_shader: prepared SPIR-V from %s", prepared_dir);
}

static void prepared_paths(void) {
    static PlatOnce once = PLAT_ONCE_INIT;
    plat_once(&once, prepared_init, NULL);
}

static GfxShader *prepared_load(const uint8_t key[32], const uint8_t *control) {
    char hex[65], p[PATH_CAP + 80];
    key_hex(key, hex);
    uint64_t spv_size = 0, res_size = 0;
    snprintf(p, sizeof p, "%s/%s.res.json", prepared_dir, hex);
    uint8_t *res = read_file(p, &res_size);
    if (!res || !reflection_valid((const char *)res, res_size)) {
        free(res);
        return NULL;
    }
    snprintf(p, sizeof p, "%s/%s.spv", prepared_dir, hex);
    uint8_t *spv = read_file(p, &spv_size);
    uint32_t magic = 0;
    if (spv && spv_size >= 20) memcpy(&magic, spv, 4);
    if (!spv || spv_size % 4 || magic != 0x07230203 || !spirv_complete((const uint32_t *)spv, spv_size / 4)) {
        free(res), free(spv);
        return NULL;
    }
    GfxShader *s = calloc(1, sizeof *s);
    memcpy(s->key, key, 32);
    memcpy(s->iid, key, 32);   // pipeline key records name a prepared shader by its program key (gfx_shader_load_input)
    s->spirv = (uint32_t *)spv;
    s->words = spv_size / 4;
    s->reflection = (char *)res;
    s->reflection_size = res_size;
    if (control) {
        uint16_t st;
        memcpy(&st, control + 0x56, 2);   // NVNshaderStage (nvn2_program_stage): 5 compute
        if (st == 5) {
            s->cs_table[0] = control[0xb1], s->cs_table[1] = control[0xb2];
            s->cs_table[2] = control[0xb3], s->cs_table[3] = control[0xb6];
            s->cs_table[4] = control[0xa9];
            s->cs_known = 1;
        }
    }
    reflect(s);
    return mem_insert(s);
}

// ---- integer vertex attributes and colour outputs (gfx_spirv_io.c)

// The variant of s with the Input (out 0) or Output (out 1) Locations in um / sm as uint / int vectors, or s.
static const GfxShader *int_io_variant(GfxShader *s, uint32_t um, uint32_t sm, int out) {
    if (!um && !sm) return s;
    uint8_t key[32];
    memcpy(key, s->key, 32);
    // variants: the program key with the masks folded into its last bytes, other marks for outputs
    key[30] ^= out ? 0x69 : 0x5a, key[31] ^= out ? 0x96 : 0xa5;
    for (int i = 0; i < 4; i++) key[22 + i] ^= (uint8_t)(um >> 8 * i), key[26 + i] ^= (uint8_t)(sm >> 8 * i);
    GfxShader *v = mem_find(key);
    if (v) return v;
    uint64_t nw = 0;
    uint32_t *code = spirv_int_io(s->spirv, s->words, um, sm, out ? 3 : 1, &nw);
    if (!code) {
        static volatile int32_t n_fail;
        if (plat_inc32(&n_fail) <= 8)
            rt_log("gfx_shader: integer %s %#x/%#x not patched in", out ? "colour outputs" : "vertex inputs", um, sm);
        return s;
    }
    if (!(v = calloc(1, sizeof *v)) || !(v->reflection = malloc(s->reflection_size + 1))) rt_fatal("gfx_shader: out of memory");
    memcpy(v->key, key, 32), memcpy(v->iid, s->iid, 32);
    v->spirv = code, v->words = nw;
    memcpy(v->reflection, s->reflection, s->reflection_size), v->reflection[s->reflection_size] = 0;
    v->reflection_size = s->reflection_size;
    reflect(v);
    for (int l = 0; l < 32; l++)
        if ((um | sm) >> l & 1) (out ? v->out : v->in)[l].type = um >> l & 1 ? GFX_TYPE_UINT : GFX_TYPE_SINT;
    const char *dd = cfg_get("MKW_SPIRV_INT_DUMP");   // the variants for spirv-val
    if (dd && *dd) {
        char path[PATH_CAP + 96], hex[65];
        key_hex(key, hex);
        plat_file_mkdir_p(dd);
        snprintf(path, sizeof path, "%s/%s.spv", dd, hex);
        plat_file_write(path, code, nw * 4);
    }
    static volatile int32_t n_var[2];   // apart: the vertex ones would use up the fragment ones' lines
    if (plat_inc32(&n_var[out]) <= 8)
        rt_log("gfx_shader: %s uint %#x sint %#x", out ? "fragment shader variant with integer outputs"
                                                       : "vertex shader variant with integer inputs", um, sm);
    return mem_insert(v);
}

// The variant of a prepared vertex shader for the integer attribute formats in a (GfxShaderAnswers.attr_type), or s.
static const GfxShader *int_input_variant(GfxShader *s, const GfxShaderAnswers *a) {
    if (!s || !a || s->stage != GFX_STAGE_VS || s->mesh_kind != GFX_MESH_NONE) return s;
    uint32_t um = 0, sm = 0;
    for (int l = 0; l < 16; l++)
        if (s->in_mask >> l & 1) {
            if (a->attr_type[l] == GFX_TYPE_UINT) um |= 1u << l;
            else if (a->attr_type[l] == GFX_TYPE_SINT) sm |= 1u << l;
        }
    return int_io_variant(s, um, sm, 0);
}

// The variant of a prepared fragment shader for the integer colour targets in a (GfxShaderAnswers.out_type), or s:
// only the outputs the shader writes (the others are masked off, vk_pipeline.c target_wmask).
static const GfxShader *int_output_variant(GfxShader *s, const GfxShaderAnswers *a) {
    if (!s || !a || s->stage != GFX_STAGE_FS) return s;
    uint32_t um = 0, sm = 0;
    for (int l = 0; l < 8; l++)
        if (s->out_mask >> l & 1) {
            if (a->out_type[l] == GFX_TYPE_UINT) um |= 1u << l;
            else if (a->out_type[l] == GFX_TYPE_SINT) sm |= 1u << l;
        }
    return int_io_variant(s, um, sm, 1);
}

static const GfxShader *int_image_variant(GfxShader *s, const GfxShaderAnswers *a);

// The integer variant a draw or dispatch's answers ask of s (none for the other stages).
static const GfxShader *int_variant(GfxShader *s, const GfxShaderAnswers *a) {
    if (!s) return s;
    return s->stage == GFX_STAGE_CS ? int_image_variant(s, a)
         : s->stage == GFX_STAGE_FS ? int_output_variant(s, a)
                                    : int_input_variant(s, a);
}

// The prepared shader of a program, or NULL (logged once per program) when the offline translator could not do it.
static const GfxShader *prepared_get(const uint8_t *control, const uint8_t *code) {
    prepared_paths();
    uint8_t key[32];
    nvn2_program_key(control, code, key);
    GfxShader *s = mem_find(key);
    if (s) return s;
    s = prepared_load(key, control);
    if (!s) {
        char hex[65];
        key_hex(key, hex);
        static volatile int32_t n_missing;
        int32_t n = plat_inc32(&n_missing);
        if (n <= 20 || !(n & (n - 1))) rt_log("gfx_shader: no prepared SPIR-V %s (%d lookups so far)", hex, n);
        // MKW_SPIRV_DUMP=<dir>: the missing program's control section and code, to find where it comes from.
        const char *dump = cfg_get("MKW_SPIRV_DUMP");
        if (dump && *dump) {   // every missing program once (the file exists after the first time)
            char p[PATH_CAP + 80];
            uint32_t ctl_size = 0xd8, size;
            memcpy(&size, control + 0x44, 4);
            plat_file_mkdir_p(dump);
            snprintf(p, sizeof p, "%s/%s.ctl", dump, hex);
            if (plat_file_exists(p)) return s;
            plat_file_write(p, control, ctl_size);
            snprintf(p, sizeof p, "%s/%s.code", dump, hex);
            plat_file_write(p, code, size < (16u << 20) ? size : 0);
        }
    }
    return s;
}

// ---- reflection: the .res.json the translator writes (tools/shaders/translate.py) and the SPIR-V itself

static const char *const fmt_names[] = {   // img_format codes: Ryujinx TextureFormat enum order
    NULL, "R8Unorm", "R8Snorm", "R8Uint", "R8Sint", "R16Float", "R16Unorm", "R16Snorm", "R16Uint", "R16Sint", "R32Float",
    "R32Uint", "R32Sint", "R8G8Unorm", "R8G8Snorm", "R8G8Uint", "R8G8Sint", "R16G16Float", "R16G16Unorm", "R16G16Snorm",
    "R16G16Uint", "R16G16Sint", "R32G32Float", "R32G32Uint", "R32G32Sint", "R8G8B8A8Unorm", "R8G8B8A8Snorm",
    "R8G8B8A8Uint", "R8G8B8A8Sint", "R16G16B16A16Float", "R16G16B16A16Unorm", "R16G16B16A16Snorm", "R16G16B16A16Uint",
    "R16G16B16A16Sint", "R32G32B32A32Float", "R32G32B32A32Uint", "R32G32B32A32Sint", "R10G10B10A2Unorm",
    "R10G10B10A2Uint", "R11G11B10Float"};
#define COUNT(a) ((int)(sizeof a / sizeof *a))

// `"name":` inside [o, e): the first byte after the colon and its spaces, NULL when absent.
static const char *json_key(const char *o, const char *e, const char *name) {
    char pat[40];
    int n = snprintf(pat, sizeof pat, "\"%s\":", name);
    for (const char *p = o; p + n <= e; p++)
        if (*p == '"' && !memcmp(p, pat, (size_t)n)) {
            p += n;
            while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
            return p;
        }
    return NULL;
}

static long json_int(const char *o, const char *e, const char *name, long def) {
    const char *p = json_key(o, e, name);
    return p ? strtol(p, NULL, 10) : def;
}

// Copies a string value into buf (empty when absent or not a string).
static void json_str(const char *o, const char *e, const char *name, char *buf, size_t n) {
    const char *p = json_key(o, e, name);
    size_t k = 0;
    if (p && *p == '"')
        for (p++; p < e && *p != '"' && k + 1 < n; p++) buf[k++] = *p;
    buf[k] = 0;
}

// The objects of the flat array `"name": [ {...}, ... ]`: calls fn for each object's [start, end). Returns the count.
static int json_objects(const char *js, const char *name, void (*fn)(GfxShader *, int, const char *, const char *),
                        GfxShader *s, int kind) {
    const char *end = js + strlen(js), *p = json_key(js, end, name);
    if (!p || *p != '[') return 0;
    int n = 0;
    for (p++; p < end && *p != ']'; p++) {
        if (*p != '{') continue;
        const char *q = memchr(p, '}', (size_t)(end - p));
        if (!q) break;
        if (fn) fn(s, kind, p, q);
        n++;
        p = q;
    }
    return n;
}

static int fmt_code(const char *name) {
    for (int i = 1; i < COUNT(fmt_names); i++)
        if (!strcmp(fmt_names[i], name)) return i;
    return 0;
}

// c2 slot of a texture or image handle (Service.cs of MNG's mkw_xlate: combined handles at 0x20, 32 slots; images at
// 0x120; separate textures at 0x168), -1 outside those ranges or for a handle in another constant buffer.
static int handle_slot(int kind, int cbuf_slot, int handle, int separate) {
    if (cbuf_slot >= 0 && cbuf_slot != 2) return -1;
    int bytes = (handle & 0x3fff) * 4;
    if (kind == GFX_RES_IMG) return bytes >= 0x120 && bytes < 0x168 ? (bytes - 0x120) / 8 : -1;
    if (separate) return bytes >= 0x168 && bytes < 0x568 ? (bytes - 0x168) / 8 : -1;
    return bytes >= 0x20 && bytes < 0x120 ? (bytes - 0x20) / 8 : -1;
}

// NVN2 compute programs read their resources from a table the driver packs into c0 for each program: from byte 0x10,
// A uniform buffers and then B storage buffers as {u64 address, u32 size} in 16 bytes, then C texture and D image
// handles in 8 bytes, each kind indexed by its NVN slot (the counts cover slots 0..n-1, unused ones included). The
// counts sit in the program's control section at +0xb1, +0xb2, +0xb3 and +0xb6, the table's end at +0xba (u16):
// measured on 14 of MKW's compute programs against their reflection, 31/10/2026 (A=13, B=10, C=19: textures at
// 0x180-0x210, end 0x218). NVN1 kept fixed tables (storage 0x310, uniform 0x210, handles in bank 2); slots here are
// given in those terms so the replay's lookups (ssbo_slot, handle_slot) stay as they are. Returns 0 when byte is not
// in the table of kind. The push constants come first, control +0xa9 words from 0x10: with them the table ends at +0xba
// in all 728 compute programs, without them in 698 (the other 30 read their tables 0x20 off). nxnative
// docs/reference/shaders.md «Ampere shader code» @ 2a6f9b3 (MIT); also MKW-THWOMP's nvn2_c0.h (GPL, the idea only).
static int cs_res(const GfxShader *s, int kind, int byte, int *slot) {
    int ub = 0x10 + 4 * s->cs_table[4], sb = ub + 16 * s->cs_table[0], tb = sb + 16 * s->cs_table[1], ib = tb + 8 * s->cs_table[2];
    int end = ib + 8 * s->cs_table[3];
    if (kind == GFX_RES_SSBO && byte >= ub && byte < sb) return *slot = (byte - ub) / 16, 2;   // a uniform buffer read as storage
    if (kind == GFX_RES_SSBO && byte >= sb && byte < tb) return *slot = (byte - sb) / 16, 1;
    if (kind == GFX_RES_TEX && byte >= tb && byte < ib) return *slot = (byte - tb) / 8, 1;
    if (kind == GFX_RES_IMG && byte >= ib && byte < end) return *slot = (byte - ib) / 8, 1;
    return 0;
}

static void add_res(GfxShader *s, int kind, const char *o, const char *e) {
    GfxShaderRes *r = &s->res[kind][s->nres[kind]++];
    r->set = 0;
    r->binding = (uint16_t)json_int(o, e, "binding", 0);
    if (kind == GFX_RES_UBO || kind == GFX_RES_SSBO) {
        r->slot = (int16_t)json_int(o, e, "slot", -1);
        r->count = 1;
        r->flags = (uint8_t)json_int(o, e, "flags", 0);   // UBO: 2, the bank's bounded word view (head.py cbNw)
        if (kind == GFX_RES_SSBO) {
            r->cbuf_slot = (int8_t)json_int(o, e, "cb_slot", -1);
            r->handle = (uint16_t)json_int(o, e, "cb_offset", 0);
            int slot, k;
            if (s->cs_known && r->cbuf_slot == 0 && (k = cs_res(s, kind, r->handle * 4, &slot)))
                r->handle = (uint16_t)((k == 2 ? 0x210 : 0x310) + 16 * slot) / 4;   // NVN1's tables (ssbo_slot)
        }
        return;
    }
    char buf[32];
    json_str(o, e, "format", buf, sizeof buf);
    const char *sep = json_key(o, e, "separate");
    r->type = (uint16_t)json_int(o, e, "type", 0);
    r->format = (uint8_t)fmt_code(buf);
    r->flags = (uint8_t)json_int(o, e, "flags", 0);
    r->cbuf_slot = (int8_t)json_int(o, e, "cbuf_slot", -1);
    int handle = (int)json_int(o, e, "handle", 0);
    r->handle = (uint16_t)handle;
    r->count = (uint16_t)json_int(o, e, "array", 1);
    r->separate = sep && *sep == 't';
    r->slot = (int16_t)handle_slot(kind, r->cbuf_slot, handle, r->separate);
    int slot;
    if (s->cs_known && r->cbuf_slot == 0 && cs_res(s, kind, handle * 4, &slot)) r->slot = (int16_t)slot;
}

// SPIR-V opcodes and enumerants used below (SPIR-V 1.6 specification, section 3).
enum { OP_EXECUTION_MODE = 16, OP_TYPE_INT = 21, OP_TYPE_FLOAT = 22, OP_TYPE_VECTOR = 23,
       OP_TYPE_ARRAY = 28, OP_TYPE_POINTER = 32, OP_VARIABLE = 59, OP_DECORATE = 71,
       DEC_LOCATION = 30, DEC_COMPONENT = 31, SC_INPUT = 1, SC_OUTPUT = 3, EM_LOCAL_SIZE = 17 };

// Interface Locations and the compute workgroup size from the module. Ids beyond 2^20 are ignored (Ryujinx modules stay
// far below).
static void reflect_spirv(GfxShader *s) {
    const uint32_t *w = s->spirv;
    uint64_t n = s->words;
    s->local[0] = s->local[1] = s->local[2] = 1;
    if (n < 5) return;
    uint32_t bound = w[3] < (1u << 20) ? w[3] : (1u << 20);
    // per id: location + 1 (0 = none), component flag, scalar/vector description of types, pointee of pointers
    int32_t *loc = calloc(bound, sizeof *loc);
    uint8_t *packed = calloc(bound, 1), *ttype = calloc(bound, 1), *tcomps = calloc(bound, 1);
    uint32_t *pointee = calloc(bound, sizeof *pointee);
    if (!loc || !packed || !ttype || !tcomps || !pointee) goto out;
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n) break;
        const uint32_t *a = w + i + 1;
        uint32_t id = len > 1 ? a[0] : 0;
        if (op == OP_EXECUTION_MODE && len >= 6 && a[1] == EM_LOCAL_SIZE) {
            s->local[0] = a[2], s->local[1] = a[3], s->local[2] = a[4];
        } else if (op == OP_DECORATE && len >= 4 && id < bound) {
            if (a[1] == DEC_LOCATION) loc[id] = (int32_t)a[2] + 1;
            else if (a[1] == DEC_COMPONENT) packed[id] = 1;
        } else if (op == OP_TYPE_INT && len >= 4 && id < bound) {
            ttype[id] = a[2] ? GFX_TYPE_SINT : GFX_TYPE_UINT, tcomps[id] = 1;
        } else if (op == OP_TYPE_FLOAT && len >= 3 && id < bound) {
            ttype[id] = GFX_TYPE_FLOAT, tcomps[id] = 1;
        } else if (op == OP_TYPE_VECTOR && len >= 4 && id < bound && a[1] < bound) {
            ttype[id] = ttype[a[1]], tcomps[id] = (uint8_t)a[2];
        } else if (op == OP_TYPE_ARRAY && len >= 4 && id < bound && a[1] < bound) {
            ttype[id] = ttype[a[1]], tcomps[id] = tcomps[a[1]];   // element type (arrayed interface blocks)
        } else if (op == OP_TYPE_POINTER && len >= 4 && id < bound) {
            pointee[id] = a[2];
        } else if (op == OP_VARIABLE && len >= 4) {
            uint32_t type = a[0], var = a[1], sc = a[2];
            if ((sc == SC_INPUT || sc == SC_OUTPUT) && var < bound && type < bound && loc[var] > 0 && loc[var] <= 32) {
                int l = loc[var] - 1;
                uint32_t t = pointee[type] < bound ? pointee[type] : 0;
                GfxShaderIo io = {ttype[t], tcomps[t] ? tcomps[t] : 4, packed[var]};
                if (sc == SC_INPUT) s->in_mask |= 1u << l, s->in[l] = io;
                else s->out_mask |= 1u << l, s->out[l] = io;
            }
        }
        i += len;
    }
out:
    free(loc), free(packed), free(ttype), free(tcomps), free(pointee);
}

// "spec": [[bank, byte], ...], the fixed-address constant words in specialization order (translator head.py). Two
// passes over the list: count the pairs, then fill.
static int spec_pairs(const char *p, const char *end, uint32_t *out) {
    int depth = 0, n = 0, nv = 0;
    long v[2] = {0, 0};
    for (p++; p < end; p++) {
        if (*p == '[') {
            depth++, nv = 0;
        } else if (*p == ']') {
            if (!depth--) break;
            if (nv == 2 && out) out[n] = (uint32_t)(v[0] << 16 | (v[1] & 0xffff));
            n += nv == 2;
        } else if (depth && *p >= '0' && *p <= '9') {
            char *q;
            long x = strtol(p, &q, 10);
            if (nv < 2) v[nv] = x;
            nv++;
            p = q - 1;
        }
    }
    return n;
}

static void reflect_spec(GfxShader *s) {
    const char *end = s->reflection + strlen(s->reflection), *p = json_key(s->reflection, end, "spec");
    if (!p || *p != '[') return;
    int n = spec_pairs(p, end, NULL);
    if (!n) return;
    s->spec = malloc(sizeof *s->spec * (size_t)n);
    if (!s->spec) rt_fatal("gfx_shader: out of memory");
    s->nspec = spec_pairs(p, end, s->spec);
}

static void reflect(GfxShader *s) {
    static const char *const kinds[GFX_RES_KINDS] = {"cbufs", "sbufs", "textures", "images"};
    const char *js = s->reflection;
    char stage[8];
    json_str(js, js + strlen(js), "stage", stage, sizeof stage);
    s->stage = !strcmp(stage, "cs") ? GFX_STAGE_CS : !strcmp(stage, "ps") ? GFX_STAGE_FS : GFX_STAGE_VS;
    s->mesh_kind = !strcmp(stage, "task") ? GFX_MESH_TASK : !strcmp(stage, "mesh") ? GFX_MESH_MESH : GFX_MESH_NONE;
    for (int k = 0; k < GFX_RES_KINDS; k++) {
        int count = json_objects(js, kinds[k], NULL, s, k);
        s->res[k] = calloc(count ? count : 1, sizeof *s->res[k]);
        json_objects(js, kinds[k], add_res, s, k);
    }
    reflect_spec(s);
    reflect_spirv(s);
}

// ---- public

// The prepared shader of a program, as its integer-input variant when a vertex shader's attributes in a have integer
// formats; NULL when the program has no prepared SPIR-V (logged).
// The variant of a prepared compute shader for the integer-format views in a (GfxShaderAnswers.img_uint/img_sint), or
// s (gfx_spirv_img.c).
static const GfxShader *int_image_variant(GfxShader *s, const GfxShaderAnswers *a) {
    if (!s || !a || s->stage != GFX_STAGE_CS || !(a->img_uint | a->img_sint)) return s;
    uint8_t key[32];
    memcpy(key, s->key, 32);
    key[30] ^= 0x3c, key[31] ^= 0xc3;   // as int_input_variant's, with other marks
    for (int i = 0; i < 8; i++) key[i + 14] ^= (uint8_t)(a->img_uint >> 8 * i), key[i + 6] ^= (uint8_t)(a->img_sint >> 8 * i);
    GfxShader *v = mem_find(key);
    if (v) return v;
    uint64_t nw = 0;
    const char *why = NULL;
    uint32_t *code = spirv_int_images(s->spirv, s->words, a->img_uint, a->img_sint, &nw, &why);
    if (!code) {
        static volatile int32_t n_fail;
        if (plat_inc32(&n_fail) <= 8) {
            char hex[65];
            key_hex(s->key, hex);
            rt_log("gfx_shader: integer images %#llx/%#llx not patched in program %s: %s; using original shader",
                   (unsigned long long)a->img_uint, (unsigned long long)a->img_sint, hex, why ? why : "unknown reason");
            // Keep the failing input too: successful variants alone cannot reproduce a failed conversion offline.
            const char *dd = cfg_get("MKW_SPIRV_INT_DUMP");
            if (dd && *dd) {
                char path[PATH_CAP + 128];
                plat_file_mkdir_p(dd);
                snprintf(path, sizeof path, "%s/%s-u%llx-s%llx.failed.spv", dd, hex,
                         (unsigned long long)a->img_uint, (unsigned long long)a->img_sint);
                plat_file_write(path, s->spirv, s->words * 4);
            }
        }
        return s;
    }
    if (!(v = calloc(1, sizeof *v)) || !(v->reflection = malloc(s->reflection_size + 1))) rt_fatal("gfx_shader: out of memory");
    memcpy(v->key, key, 32), memcpy(v->iid, s->iid, 32);
    v->spirv = code, v->words = nw;
    memcpy(v->reflection, s->reflection, s->reflection_size), v->reflection[s->reflection_size] = 0;
    v->reflection_size = s->reflection_size;
    memcpy(v->cs_table, s->cs_table, sizeof v->cs_table), v->cs_known = s->cs_known;   // reflect places the resources by it
    reflect(v);
    const char *dd = cfg_get("MKW_SPIRV_INT_DUMP");   // the variants for spirv-val
    if (dd && *dd) {
        char path[PATH_CAP + 96], hex[65];
        key_hex(key, hex);
        plat_file_mkdir_p(dd);
        snprintf(path, sizeof path, "%s/%s.spv", dd, hex);
        plat_file_write(path, code, nw * 4);
    }
    static volatile int32_t n_var;
    if (plat_inc32(&n_var) <= 8)
        rt_log("gfx_shader: compute shader variant with integer images uint %#llx sint %#llx", (unsigned long long)a->img_uint,
               (unsigned long long)a->img_sint);
    return mem_insert(v);
}

const GfxShader *gfx_shader_get(const uint8_t *control, const uint8_t *code, const GfxShaderAnswers *a) {
    GfxShader *s = (GfxShader *)prepared_get(control, code);
    return int_variant(s, a);
}

uint64_t gfx_shader_count(void) { return (uint64_t)n_keys; }

const uint8_t *gfx_shader_key(const GfxShader *s) { return s->key; }

const uint8_t *gfx_shader_input_id(const GfxShader *s) { return s->iid; }

// By program key (a pipeline key record's shader id): memory, then the prepared directory. NULL when neither has it.
const GfxShader *gfx_shader_load_input(const uint8_t iid[32]) {
    prepared_paths();
    GfxShader *s = mem_find(iid);
    return s ? s : prepared_load(iid, NULL);
}

// The shaders named by pipeline key records, loaded (out[i] NULL when one is missing). Off from run49 to 06/11/2026: the
// records' integer-input vertex shaders were built from the base program (vk_pipe_keys.c), and the key file was never
// read after the TOTK magic rename (05/11/2026).
// read on every core: one by one, 729 shaders took 1 056 ms the guest waited for (pw2, 06/11/2026)
static struct { const uint8_t (*iids)[32]; const GfxShader **out; size_t n; volatile int64_t next, miss; } PW;

static void prewarm_worker(void *arg) {
    (void)arg;
    for (int64_t i; (i = plat_inc64(&PW.next) - 1) < (int64_t)PW.n;)
        if (!(PW.out[i] = gfx_shader_load_input(PW.iids[i]))) plat_inc64(&PW.miss);
}

void gfx_shader_prewarm(const uint8_t (*iids)[32], size_t n, const GfxShader **out) {
    PW.iids = iids, PW.out = out, PW.n = n, PW.next = 0, PW.miss = 0;
    PlatThread *t[64];
    int nt = 0, cpus = (int)plat_cpu_count();
    for (int i = 0; i < cpus && i < 64 && n > 16; i++)
        if ((t[nt] = plat_thread_new(prewarm_worker, NULL, "shader prewarm"))) nt++;
    prewarm_worker(NULL);
    for (int i = 0; i < nt; i++)
        while (!plat_thread_join_ns(t[i], 1000000000u)) {}
    rt_log("gfx_shader: prewarm: %zu shaders named by pipeline keys, %lld not found, on %d threads", n,
           (long long)PW.miss, nt + 1);
}

const GfxShader *gfx_shader_int_variant(const GfxShader *s, const GfxShaderAnswers *a) {
    return int_variant((GfxShader *)s, a);
}

const uint32_t *gfx_shader_spirv(const GfxShader *s, uint64_t *words) {
    if (words) *words = s->words;
    return s->spirv;
}

int gfx_shader_stage(const GfxShader *s) { return s->stage; }

// A copy of s whose specialization constants default to val[k] for each word k with fix[k] (SCU<k> true, SCV<k> =
// val[k]), the same as spirv-opt --set-spec-const-default-value: the pipeline code sees an ordinary shader, so the
// pipeline cache, the GPL libraries and the background builds treat a variant like any other. The copy lives for
// the process and shares s's reflection; its key and input id are sha256(s's key, the fixed words). NULL when the
// SPIR-V has none of the constants.
const GfxShader *gfx_shader_variant(const GfxShader *s, const uint32_t *val, const uint8_t *fix) {
    enum { OP_SPEC_TRUE = 48, OP_SPEC_FALSE = 49, OP_SPEC = 50, DEC_SPEC_ID = 1 };
    if (s->words < 5) return NULL;
    uint32_t bound = s->spirv[3], *w = malloc(sizeof *w * s->words);
    int32_t *sid = malloc(sizeof *sid * (bound ? bound : 1));
    if (!w || !sid) rt_fatal("gfx_shader: out of memory");
    memcpy(w, s->spirv, sizeof *w * s->words);
    memset(sid, 0xff, sizeof *sid * bound);
    for (uint64_t i = 5; i < s->words;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > s->words) break;
        if (op == OP_DECORATE && len >= 4 && w[i + 2] == DEC_SPEC_ID && w[i + 1] < bound) sid[w[i + 1]] = (int32_t)w[i + 3];
        i += len;
    }
    int patched = 0;
    for (uint64_t i = 5; i < s->words;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > s->words) break;
        if ((op == OP_SPEC_TRUE || op == OP_SPEC_FALSE || op == OP_SPEC) && len >= 3 && w[i + 2] < bound) {
            int32_t id = sid[w[i + 2]];
            int k = id >> 1;
            if (id >= 0 && k < s->nspec && fix[k]) {
                if (!(id & 1) && op != OP_SPEC) w[i] = (w[i] & 0xffff0000u) | OP_SPEC_TRUE, patched++;
                else if ((id & 1) && op == OP_SPEC && len == 4) w[i + 3] = val[k], patched++;
            }
        }
        i += len;
    }
    free(sid);
    if (!patched) {
        free(w);
        return NULL;
    }
    GfxShader *v = malloc(sizeof *v);
    if (!v) rt_fatal("gfx_shader: out of memory");
    *v = *s;
    v->next = NULL, v->spirv = w, v->variant_of = s;
    size_t n = 32 + (size_t)s->nspec * 5;
    uint8_t *buf = malloc(n);
    if (!buf) rt_fatal("gfx_shader: out of memory");
    memcpy(buf, s->key, 32);
    for (int k = 0; k < s->nspec; k++) buf[32 + 5 * k] = fix[k], memcpy(buf + 33 + 5 * k, fix[k] ? &val[k] : &(uint32_t){0}, 4);
    rt_sha256(buf, n, v->key);
    memcpy(v->iid, v->key, 32);
    free(buf);
    return v;
}

const GfxShader *gfx_shader_variant_of(const GfxShader *s) { return s->variant_of; }

int gfx_shader_spec(const GfxShader *s, const uint32_t **words) {
    *words = s->spec;
    return s->nspec;
}

int gfx_shader_resources(const GfxShader *s, int kind, const GfxShaderRes **res) {
    if (kind < 0 || kind >= GFX_RES_KINDS) return 0;
    if (res) *res = s->res[kind];
    return s->nres[kind];
}

uint32_t gfx_shader_inputs(const GfxShader *s, GfxShaderIo io[32]) {
    if (io) memcpy(io, s->in, sizeof s->in);
    return s->in_mask;
}

uint32_t gfx_shader_outputs(const GfxShader *s, GfxShaderIo io[32]) {
    if (io) memcpy(io, s->out, sizeof s->out);
    return s->out_mask;
}

int gfx_shader_mesh_kind(const GfxShader *s) { return s ? s->mesh_kind : GFX_MESH_NONE; }

int gfx_shader_cs_of_mesh(const GfxShader *s) { return s && s->cs_of_mesh; }

// The vertex shader of task/mesh as compute: a prepared shader under a fixed key that no program has
// (tools/shaders/translate.py write_mesh_cs_vs, sm86 head.py MESH_CS_VS_KEY), so pipeline key records name it too.
static const uint8_t mesh_cs_vs_key[32] = "mkw-mesh-cs-vs-1mkw-mesh-cs-vs-1";

const GfxShader *gfx_shader_mesh_cs_vs(void) {
    static const GfxShader *vs;
    static int tried;
    if (!tried) {
        vs = gfx_shader_load_input(mesh_cs_vs_key), tried = 1;
        if (!vs) rt_log("gfx_shader: no vertex shader for task/mesh as compute (spirv/%s.spv)", "6d6b772d...");
    }
    return vs;
}

int gfx_shader_is_mesh_cs_vs(const GfxShader *s) { return s && !memcmp(s->key, mesh_cs_vs_key, 32); }

// The compute form of a task or mesh program, <key>.cs.spv beside its .spv, with the
// same reflection: stage CS, the task/mesh kind kept for its binding tags. Loaded once and kept with the program (not
// in the cache by key, where the program itself is). NULL for any other program or when the file is missing.
const GfxShader *gfx_shader_cs_variant(const GfxShader *s) {
    static PlatMutex lock = PLAT_MUTEX_INIT;
    if (!s || s->mesh_kind == GFX_MESH_NONE || s->cs_of_mesh) return NULL;
    GfxShader *m = (GfxShader *)s;
    plat_mutex_lock(&lock);
    if (!m->cs_variant) {
        char hex[65], p[PATH_CAP + 80];
        key_hex(s->key, hex);
        snprintf(p, sizeof p, "%s/%s.cs.spv", prepared_dir, hex);
        uint64_t n = 0;
        uint8_t *spv = read_file(p, &n);
        uint32_t magic = 0;
        if (spv && n >= 20) memcpy(&magic, spv, 4);
        char *res = malloc(s->reflection_size + 1);
        if (spv && n % 4 == 0 && magic == 0x07230203 && spirv_complete((const uint32_t *)spv, n / 4) && res) {
            GfxShader *v = calloc(1, sizeof *v);
            memcpy(v->key, s->key, 32), memcpy(v->iid, s->iid, 32);
            v->spirv = (uint32_t *)spv, v->words = n / 4;
            memcpy(res, s->reflection, s->reflection_size), res[s->reflection_size] = 0;
            v->reflection = res, v->reflection_size = s->reflection_size;
            reflect(v);
            v->stage = GFX_STAGE_CS, v->cs_of_mesh = 1;
            m->cs_variant = v;
        } else {
            free(spv), free(res);
            rt_log("gfx_shader: no compute form of %s program %s (%s)", s->mesh_kind == GFX_MESH_TASK ? "task" : "mesh", hex, p);
        }
    }
    plat_mutex_unlock(&lock);
    return m->cs_variant;
}
const uint8_t *gfx_shader_cs_table(const GfxShader *s) { return s && s->cs_known ? s->cs_table : NULL; }
