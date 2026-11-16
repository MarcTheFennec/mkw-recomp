// The game's zstd decoder on the host's libzstd. Most RomFS files are zstd frames, many with a dictionary; the decoder
// is BSD zstd linked into main. The patch set "zstd" (recomp/patches-mkw.txt) sends both calls of MKW's
// ZSTD_decompressMultiFrame (1.0.0: main+0x407e00) to the host function below, on the patch page (totk-pc-recomp also patched
// ZSTD_decompress_usingDDict; MKW reaches it through MultiFrame). Lifted, the decoder ran its literal and match copies
// as chains of small functions with the registers in Cpu (run50: 18.5 % of the CPU); here they decode in native code.
//
// - Results match the guest's: libzstd decodes the same format, and every result that is an error goes to the guest
//   decoder again (it writes its own error code; a frame libzstd rejects may still be one the game's version accepts).
// - A guest ZSTD_DDict (zstd's struct: dictBuffer, dictContent, dictSize first) becomes a host ZSTD_DDict made from a
//   copy of its content, cached by guest address, content address, size and a fingerprint of the content.
// - Inputs of at least AWAY_BYTES give the virtual core up while they decode (other guest threads run on it).
// MKW_ZSTD_NATIVE=0 keeps the guest decoder (the patch set switch); MKW_ZSTD_CHECK=1 decodes each call both ways,
// keeps the guest's result and logs every call where the two differ.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>

#include "cfg.h"
#include "hacks.h"
#include "host.h"
#include "version.h"
#include "platform/platform.h"

// ZSTD_decompressMultiFrame (1.0.0: main+0x407e00, 1.8.0: main+0xe2280; version.c, the patch set "zstd")
#define GUEST_MULTIFRAME rt_main_addr(rt_version()->zstd_multiframe)
#define HOST_MULTIFRAME 0xff0   // offsets in the patch page
#define AWAY_BYTES (64u << 10)
#define MAX_DICT (64u << 20)
#define NDICT 32

static void *gp(uint64_t a) { return (void *)(uintptr_t)a; }

typedef struct {
    uint64_t guest, content, size, print;
    ZSTD_DDict *dd;
} Dict;
static Dict dicts[NDICT];
static int ndicts;
static PlatMutex dict_lock = PLAT_MUTEX_INIT;

static struct {
    uint64_t calls, in, out, ns, guest_ns, fallbacks, away, inplace;
} S;

static int check_mode(void) {
    static int v = -1;
    if (v < 0) v = cfg_bool("MKW_ZSTD_CHECK");
    return v;
}

// Size, both ends and a sample of the middle: what tells a new dictionary at a reused address from the old one.
static uint64_t fingerprint(const uint8_t *p, uint64_t n) {
    uint64_t h = n * 0x9E3779B97F4A7C15ull;
    uint64_t at[3] = {0, n / 2, n > 64 ? n - 64 : 0};
    for (int k = 0; k < 3; k++)
        for (uint64_t i = at[k]; i < at[k] + 64 && i < n; i += 8) {
            uint64_t v = 0;
            memcpy(&v, p + i, n - i < 8 ? n - i : 8);
            h = (h ^ v) * 0xC2B2AE3D27D4EB4Full;
            h ^= h >> 29;
        }
    return h;
}

// The host dictionary for a guest ZSTD_DDict or raw dictionary bytes (guest == 0); NULL when its fields look wrong.
static ZSTD_DDict *dict_for(uint64_t guest, uint64_t content, uint64_t size) {
    if (!content || !size || size > MAX_DICT || content >= RT_ADDRESS_LIMIT || content + size > RT_ADDRESS_LIMIT)
        return NULL;
    uint64_t print = fingerprint(gp(content), size);
    ZSTD_DDict *dd = NULL;
    plat_mutex_lock_shared(&dict_lock);
    for (int i = 0; i < ndicts && !dd; i++)
        if (dicts[i].guest == guest && dicts[i].content == content && dicts[i].size == size && dicts[i].print == print)
            dd = dicts[i].dd;
    plat_mutex_unlock_shared(&dict_lock);
    if (dd) return dd;
    // A copy: the guest may free or reuse its buffer. Entries are never freed (another thread may be decoding with
    // one); a full table sends further dictionaries to the guest decoder.
    ZSTD_DDict *made = ZSTD_createDDict(gp(content), (size_t)size);
    if (!made) return NULL;
    int slot = -1;
    plat_mutex_lock(&dict_lock);
    for (int i = 0; i < ndicts && !dd; i++)   // another thread may have made it meanwhile
        if (dicts[i].guest == guest && dicts[i].content == content && dicts[i].size == size && dicts[i].print == print)
            dd = dicts[i].dd;
    if (!dd && ndicts < NDICT) slot = ndicts, dicts[ndicts++] = (Dict){guest, content, size, print, made}, dd = made;
    plat_mutex_unlock(&dict_lock);
    if (dd != made) {
        ZSTD_freeDDict(made);
        if (!dd) rt_log("zstd: more than %d dictionaries: further ones decode in the guest", NDICT);
        return dd;
    }
    rt_log("zstd: host dictionary %d for %s %#llx: %llu bytes at %#llx, id %u", slot, guest ? "DDict" : "raw dict",
           (unsigned long long)(guest ? guest : content), (unsigned long long)size, (unsigned long long)content,
           ZSTD_getDictID_fromDDict(dd));
    return dd;
}

// Decodes on this thread's host context. Returns libzstd's result; *ok 0 when the inputs cannot be taken.
static size_t host_decode(uint64_t dst, uint64_t cap, uint64_t src, uint64_t size, uint64_t dict, uint64_t dict_size,
                          uint64_t ddict, int *ok) {
    static __thread ZSTD_DCtx *dctx;
    *ok = 0;
    if ((cap && !dst) || !src || src + size > RT_ADDRESS_LIMIT) return 0;   // dst: decode() checks a guest one
    ZSTD_DDict *dd = NULL;
    if (ddict) {
        if (ddict >= RT_ADDRESS_LIMIT - 24) return 0;
        uint64_t f[3];   // ZSTD_DDict: dictBuffer, dictContent, dictSize
        memcpy(f, gp(ddict), sizeof f);
        if (!(dd = dict_for(ddict, f[1], f[2]))) {
            static int logged;
            if (logged < 4) {
                uint64_t w[8];
                memcpy(w, gp(ddict), sizeof w);
                logged++;
                rt_log("zstd: DDict %#llx not taken: %llx %llx %llx %llx %llx %llx %llx %llx", (unsigned long long)ddict,
                       (unsigned long long)w[0], (unsigned long long)w[1], (unsigned long long)w[2], (unsigned long long)w[3],
                       (unsigned long long)w[4], (unsigned long long)w[5], (unsigned long long)w[6], (unsigned long long)w[7]);
            }
            return 0;
        }
    } else if (dict && dict_size) {
        if (!(dd = dict_for(0, dict, dict_size))) return 0;
    }
    if (!dctx && !(dctx = ZSTD_createDCtx())) return 0;
    *ok = 1;
    return dd ? ZSTD_decompress_usingDDict(dctx, gp(dst), (size_t)cap, gp(src), (size_t)size, dd)
              : ZSTD_decompressDCtx(dctx, gp(dst), (size_t)cap, gp(src), (size_t)size);
}

// The host call inside the thread's host depth, as hostfn.c counts it for asynchronous preemption.
static size_t tracked_decode(Cpu *c, uint64_t dst, uint64_t cap, uint64_t src, uint64_t size, uint64_t dict,
                             uint64_t dict_size, uint64_t ddict, int *ok) {
    int *d = kern_host_track && c->thread ? kern_host_depth(c->thread) : NULL;
    if (d) __atomic_store_n(d, *d + 1, __ATOMIC_RELAXED);
    int away = size >= AWAY_BYTES ? kern_core_away() : 0;
    if (away) __atomic_add_fetch(&S.away, 1, __ATOMIC_RELAXED);
    size_t r = host_decode(dst, cap, src, size, dict, dict_size, ddict, ok);
    kern_core_back(&away);
    if (d) {
        int n = *d - 1;
        __atomic_store_n(d, n, __ATOMIC_RELAXED);
        if (!n && kern_preempt_pending(c->thread)) kern_host_left(c->thread);
    }
    return r;
}

static void count(uint64_t in, uint64_t out, uint64_t ns) {
    uint64_t n = __atomic_add_fetch(&S.calls, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&S.in, in, __ATOMIC_RELAXED);
    __atomic_add_fetch(&S.out, out, __ATOMIC_RELAXED);
    __atomic_add_fetch(&S.ns, ns, __ATOMIC_RELAXED);
    if (n == 1 || !(n & 0x3fff))
        rt_log("zstd: %llu calls on the host, %.1f MiB in, %.1f MiB out, %.0f ms (check mode: the guest decoder %.0f ms); "
               "%llu to the guest decoder, %llu in place in the guest, %llu with the core given up", (unsigned long long)n,
               S.in / 1048576.0, S.out / 1048576.0, S.ns / 1e6, S.guest_ns / 1e6, (unsigned long long)S.fallbacks,
               (unsigned long long)S.inplace, (unsigned long long)S.away);
}

// x0 dctx, x1 dst, x2 capacity, x3 src, x4 size, then (dict, dict size, ddict) as the entry has them. guest: the
// guest function with the same arguments, n of them.
static void decode(Cpu *c, uint64_t dict, uint64_t dict_size, uint64_t ddict, uint64_t guest, int n) {
    uint64_t a[8];
    memcpy(a, c->x, sizeof a);
    uint64_t dst = a[1], cap = a[2], src = a[3], size = a[4];
    // In place (the input inside the output buffer): the result depends on how the decoder's writes overtake its
    // reads, which libzstd does not promise to do as the game's version does, and a host decode that failed half way
    // would leave the guest decoder a damaged input. Such calls stay in the guest.
    if (cap && src < dst + cap && dst < src + size) {
        __atomic_add_fetch(&S.inplace, 1, __ATOMIC_RELAXED);
        rt_call(c, guest, n, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
        return;
    }
    if (check_mode()) {
        uint8_t *mine = cap ? malloc((size_t)cap) : NULL;
        int ok = 0;
        uint64_t t0 = kern_now_ns();
        size_t r = cap && !mine ? 0 : tracked_decode(c, (uint64_t)(uintptr_t)mine, cap, src, size, dict, dict_size, ddict, &ok);
        uint64_t t1 = kern_now_ns();
        uint64_t g = rt_call(c, guest, n, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
        __atomic_add_fetch(&S.guest_ns, kern_now_ns() - t1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&S.ns, t1 - t0, __ATOMIC_RELAXED);
        static int logged;
        if (logged < 6 && (!ok || ZSTD_isError(r))) {
            logged++;
            rt_log("zstd check: host %s, guest %llu; dctx %#llx dst %#llx cap %llu src %#llx size %llu dict %#llx/%llu "
                   "ddict %#llx", !ok ? "inputs not taken" : ZSTD_getErrorName(r), (unsigned long long)g,
                   (unsigned long long)a[0], (unsigned long long)dst, (unsigned long long)cap, (unsigned long long)src,
                   (unsigned long long)size, (unsigned long long)dict, (unsigned long long)dict_size,
                   (unsigned long long)ddict);
        }
        if (ok && !ZSTD_isError(r) && (g != r || (r && memcmp(mine, gp(dst), r)))) {
            const uint8_t *p = gp(src);
            size_t fs = ZSTD_findFrameCompressedSize(p, (size_t)size);
            unsigned long long cs = ZSTD_getFrameContentSize(p, (size_t)size);
            ZSTD_DCtx *fresh = ZSTD_createDCtx();
            size_t r2 = ZSTD_decompressDCtx(fresh, mine, (size_t)cap, p, (size_t)size);
            ZSTD_freeDCtx(fresh);
            static int nlog;
            if (nlog++ < 8)
                rt_log("zstd check: MISMATCH %llu bytes from %#llx (cap %llu, first frame %lld bytes, content %lld, magic "
                       "%02x%02x%02x%02x, header %02x, dict %#llx/%llu, ddict %#llx, entry %s, lr %#llx): the host decoded "
                       "%llu bytes, the guest %lld%s; a fresh context without a dictionary: %s", (unsigned long long)size,
                       (unsigned long long)src, (unsigned long long)cap, ZSTD_isError(fs) ? -1ll : (long long)fs,
                       (long long)cs, p[3], p[2], p[1], p[0], p[4], (unsigned long long)dict,
                       (unsigned long long)dict_size, (unsigned long long)ddict, n == 8 ? "MultiFrame" : "usingDDict",
                       (unsigned long long)c->x[30], (unsigned long long)r, (long long)g, g == r ? " (other contents)" : "",
                       ZSTD_isError(r2) ? ZSTD_getErrorName(r2) : "decodes");
        }
        free(mine);
        count(size, ok && !ZSTD_isError(r) ? r : 0, 0);
        return;
    }
    uint64_t t0 = kern_now_ns();
    int ok = 0;
    size_t r = dst + cap > RT_ADDRESS_LIMIT ? 0 : tracked_decode(c, dst, cap, src, size, dict, dict_size, ddict, &ok);
    if (!ok || ZSTD_isError(r)) {   // the guest decoder decides (and writes its own error code)
        __atomic_add_fetch(&S.fallbacks, 1, __ATOMIC_RELAXED);
        static int logged;
        if (logged < 8) {
            logged++;
            rt_log("zstd: %llu bytes at %#llx go to the guest decoder: %s", (unsigned long long)size,
                   (unsigned long long)src, !ok ? "inputs not taken" : ZSTD_getErrorName(r));
        }
        rt_call(c, guest, n, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
        return;
    }
    c->x[0] = r;
    count(size, r, kern_now_ns() - t0);
    classic_seen(gp(dst), r);
}

static void host_multiframe(Cpu *c) { decode(c, c->x[5], c->x[6], c->x[7], GUEST_MULTIFRAME, 8); }

// --- Streaming: the loads (run55: MultiFrame saw 1 call) feed a frame in chunks as they are read, through the game's
// main+0x6532b4 (stream, src, size, ddict) -> int, called only from main+0x653274. It mirrors that function on a host
// ZSTD_DCtx per stream object: the same states, staging and results, with ZSTD_decompressContinue as the game calls
// it (each block straight into the output, so the window is the output already written, as in the guest).
// Stream object (main+0x6532b4): u8 state (1 new frame, 2 decoding, 3 failed, 4 done), +0x08 dst, +0x10 i32 dst room,
// +0x14 i32 content bytes left (the result), +0x18 guest DCtx memory, +0x20 staging, +0x28 i32 staging size,
// +0x2c i32 staged bytes, +0x30 guest DCtx. The guest DCtx keeps only its next input size (+0x7068) up to date, the
// one field the game reads. Streams the host could not take (a DDict it cannot copy, a full table) stay in the guest
// decoder from their first chunk to the end; so does every stream in check mode.
#define GUEST_STREAM rt_main_addr(rt_version()->zstd_stream)   // 1.0.0: main+0x6532b4, 1.8.0: main+0xdeca0
#define HOST_STREAM 0xfe0
#define NSTREAM 64
#define DCTX_EXPECTED 0x7068

static struct {
    uint64_t obj, start, size;   // start, size: the frame's output, for romfs_classic_seen
    ZSTD_DCtx *d;
} streams[NSTREAM];
static PlatMutex stream_lock = PLAT_MUTEX_INIT;
static struct {
    uint64_t frames, chunks, in, out, ns, guest;
} SS;

static uint8_t *gb(uint64_t a) { return (uint8_t *)(uintptr_t)a; }
static int32_t rd32(uint64_t a) { int32_t v; memcpy(&v, gb(a), 4); return v; }
static void wr32(uint64_t a, int32_t v) { memcpy(gb(a), &v, 4); }
static uint64_t rd64(uint64_t a) { uint64_t v; memcpy(&v, gb(a), 8); return v; }
static void wr64(uint64_t a, uint64_t v) { memcpy(gb(a), &v, 8); }

// The host context of a stream; create: a new one (or the old one reset) for a new frame. NULL: not a host stream.
static ZSTD_DCtx *stream_ctx(uint64_t obj, int create) {
    ZSTD_DCtx *d = NULL;
    int free_slot = -1;
    plat_mutex_lock(&stream_lock);
    for (int i = 0; i < NSTREAM && !d; i++) {
        if (streams[i].obj == obj) d = streams[i].d;
        else if (!streams[i].obj && free_slot < 0) free_slot = i;
    }
    if (!d && create && free_slot >= 0 && (d = ZSTD_createDCtx())) streams[free_slot].obj = obj, streams[free_slot].d = d;
    plat_mutex_unlock(&stream_lock);
    return d;
}

static void stream_drop(uint64_t obj) {
    plat_mutex_lock(&stream_lock);
    for (int i = 0; i < NSTREAM; i++)
        if (streams[i].obj == obj)
            ZSTD_freeDCtx(streams[i].d), streams[i].obj = streams[i].start = streams[i].size = 0, streams[i].d = NULL;
    plat_mutex_unlock(&stream_lock);
}

static void stream_output(uint64_t obj, uint64_t start, uint64_t size) {
    plat_mutex_lock(&stream_lock);
    for (int i = 0; i < NSTREAM; i++)
        if (streams[i].obj == obj) streams[i].start = start, streams[i].size = size;
    plat_mutex_unlock(&stream_lock);
}

static void stream_output_get(uint64_t obj, uint64_t *start, uint64_t *size) {
    plat_mutex_lock(&stream_lock);
    for (int i = 0; i < NSTREAM; i++)
        if (streams[i].obj == obj) *start = streams[i].start, *size = streams[i].size;
    plat_mutex_unlock(&stream_lock);
}

static int32_t stream_fail(uint64_t obj, int32_t r) {
    gb(obj)[0] = 3;
    stream_drop(obj);
    return r;
}

// One block as the guest's ZSTD_decompressContinue call would decode it; advances dst, room and content left.
static int stream_block(Cpu *c, uint64_t obj, ZSTD_DCtx *d, const void *src, size_t n, int32_t *err) {
    int *depth = kern_host_track && c->thread ? kern_host_depth(c->thread) : NULL;
    if (depth) __atomic_store_n(depth, *depth + 1, __ATOMIC_RELAXED);
    int away = n >= AWAY_BYTES ? kern_core_away() : 0;
    uint64_t dst = rd64(obj + 8);
    size_t r = ZSTD_decompressContinue(d, gp(dst), (size_t)(int64_t)rd32(obj + 0x10), src, n);
    kern_core_back(&away);
    if (depth) {
        int k = *depth - 1;
        __atomic_store_n(depth, k, __ATOMIC_RELAXED);
        if (!k && kern_preempt_pending(c->thread)) kern_host_left(c->thread);
    }
    if (ZSTD_isError(r)) {   // the guest: -2 when the output is too small, else -1
        static int logged;
        if (logged++ < 8) rt_log("zstd stream %#llx: %s", (unsigned long long)obj, ZSTD_getErrorName(r));
        *err = ZSTD_getErrorCode(r) == ZSTD_error_dstSize_tooSmall ? -2 : -1;
        return 0;
    }
    wr64(obj + 8, dst + (uint64_t)(int64_t)(int32_t)r);
    wr32(obj + 0x10, rd32(obj + 0x10) - (int32_t)r);
    wr32(obj + 0x14, rd32(obj + 0x14) - (int32_t)r);
    __atomic_add_fetch(&SS.out, r, __ATOMIC_RELAXED);
    return 1;
}

static int32_t stream_chunk(Cpu *c, uint64_t obj, const uint8_t *src, int32_t size, uint64_t ddict, int *guest) {
    uint8_t state = gb(obj)[0];
    ZSTD_DCtx *d;
    if (state == 1) {
        ZSTD_DDict *dd = NULL;
        if (ddict) {
            uint64_t f[3];   // ZSTD_DDict: dictBuffer, dictContent, dictSize
            if (ddict < RT_ADDRESS_LIMIT - 24) memcpy(f, gp(ddict), sizeof f);
            if (ddict >= RT_ADDRESS_LIMIT - 24 || !(dd = dict_for(ddict, f[1], f[2]))) return stream_drop(obj), *guest = 1, 0;
        }
        if (check_mode() || !(d = stream_ctx(obj, 1))) return stream_drop(obj), *guest = 1, 0;
        ZSTD_frameHeader h;
        if (ZSTD_getFrameHeader(&h, src, (size_t)size) || h.frameType == ZSTD_skippableFrame ||
            h.frameContentSize >= ZSTD_CONTENTSIZE_ERROR)
            return stream_fail(obj, -1);
        wr32(obj + 0x14, (int32_t)h.frameContentSize);
        stream_output(obj, rd64(obj + 8), h.frameContentSize);
        if (ZSTD_isError(dd ? ZSTD_decompressBegin_usingDDict(d, dd) : ZSTD_decompressBegin(d))) return stream_fail(obj, -1);
        uint64_t mem = rd64(obj + 0x18);
        wr64(obj + 0x30, mem & 7 ? 0 : mem);
        gb(obj)[0] = 2;
        __atomic_add_fetch(&SS.frames, 1, __ATOMIC_RELAXED);
    } else if (state == 3) {
        return -1;
    } else if (state == 4) {
        return -3;
    } else if (!(d = stream_ctx(obj, 0))) {
        return *guest = 1, 0;   // a stream the guest decoder started
    }
    uint64_t gdctx = rd64(obj + 0x30), stage = rd64(obj + 0x20);
    int32_t err = 0;
    __atomic_add_fetch(&SS.chunks, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&SS.in, (uint64_t)(size > 0 ? size : 0), __ATOMIC_RELAXED);
    while (size > 0) {
        size_t want = ZSTD_nextSrcSizeToDecompress(d);
        if (!want) return stream_fail(obj, -3);   // input past the end of the frame
        int32_t exp = (int32_t)want, staged = rd32(obj + 0x2c);
        if (staged < 1) {
            if (size < exp) {   // a block split across chunks: wait for the rest in the staging buffer
                if (rd32(obj + 0x28) < exp) return stream_fail(obj, -1);
                memcpy(gb(stage), src, (size_t)size);
                wr32(obj + 0x2c, size);
                break;
            }
            if (!stream_block(c, obj, d, src, (size_t)exp, &err)) return stream_fail(obj, err);
            src += exp, size -= exp;
            continue;
        }
        int32_t n = exp - staged < size ? exp - staged : size;
        if (n > 0) memcpy(gb(stage) + staged, src, (size_t)n), src += n, size -= n, staged += n, wr32(obj + 0x2c, staged);
        if (staged < exp) continue;
        if (!stream_block(c, obj, d, gb(stage), (size_t)exp, &err)) return stream_fail(obj, err);
        if (staged != exp) memmove(gb(stage), gb(stage) + exp, (size_t)(staged - exp));
        wr32(obj + 0x2c, staged - exp);
    }
    size_t left = ZSTD_nextSrcSizeToDecompress(d);
    if (gdctx) wr64(gdctx + DCTX_EXPECTED, left);
    if (!left) {
        uint64_t start = 0, n = 0;
        stream_output_get(obj, &start, &n);
        gb(obj)[0] = 4, stream_drop(obj);
        if (start && n) classic_seen(gp(start), (size_t)n);
    }
    return rd32(obj + 0x14);
}

static void host_stream(Cpu *c) {
    uint64_t a[4] = {c->x[0], c->x[1], c->x[2], c->x[3]};
    int guest = 0, was_done = gb(a[0])[0] == 4;
    uint64_t t0 = kern_now_ns();
    int32_t r = stream_chunk(c, a[0], gb(a[1]), (int32_t)a[2], a[3], &guest);
    if (guest) {
        __atomic_add_fetch(&SS.guest, 1, __ATOMIC_RELAXED);
        rt_call(c, GUEST_STREAM, 4, a[0], a[1], a[2], a[3]);
        return;
    }
    c->x[0] = (uint64_t)(uint32_t)r;
    __atomic_add_fetch(&SS.ns, kern_now_ns() - t0, __ATOMIC_RELAXED);
    uint64_t f = __atomic_load_n(&SS.frames, __ATOMIC_RELAXED);
    if (!was_done && gb(a[0])[0] == 4 && (f == 1 || !(f & 0x1ff)))
        rt_log("zstd stream: %llu frames, %llu chunks on the host, %.1f MiB in, %.1f MiB out, %.0f ms; %llu chunks in "
               "the guest decoder", (unsigned long long)SS.frames, (unsigned long long)SS.chunks, SS.in / 1048576.0,
               SS.out / 1048576.0, SS.ns / 1e6, (unsigned long long)SS.guest);
}

// The patch page is the game code's (gamelib.h), known only once it is loaded: patches.c rt_patch_hosts.
void zstd_patch_hosts(uint64_t page) {
    rt_register_host(page + HOST_MULTIFRAME, host_multiframe);
    rt_register_host(page + HOST_STREAM, host_stream);
}
