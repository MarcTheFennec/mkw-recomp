// Classic Grand Prix (MKW_CLASSIC_GP, Hacks tab): every race of a Grand Prix cup is laps on one course.
//
// A cup (Gyml/GrandprixCupSettingGParam/<cup>...bgyml in the RomFS archive general_format.Product_<version>.sarc.zs,
// its CourseList of 4 dictionaries) races a 3-lap course first and then three intermissions (CourseGParam
// InterStage*: one lap on the road to the next course). Classic puts in each intermission's place the course of its
// destination. The destination is the intermission's DestStageIdValue; the course of a destination is the
// NormalCourse[0] of the CourseAreaGParam whose StageType (or file name) hashes to it. Each CourseId is the 32-bit
// MurmurHash3 (seed 0) of the course's file name. Checked against "Classic Grand Prix (No Intermissions!)"
// (gamebanana.com/mods/736220) for 1.8.0: the same course in all 24 places; that mod's own code patches (cup select
// names, map lines) are not needed for the races. Only the 8 cups of GrandprixSettingGParam/Grandprix (Knockout Tour
// has its own, untouched).
//
// Nothing is written to disk. The host zstd decoder (sdk/ov/ov_zstd.c) hands each output to classic_seen; in the
// archive the CourseIds of the cups change before the game reads it, when the setting is on at start. The game reads
// the cups once, into objects of its own, so a change in the menu later writes the CourseIds where those objects
// hold them: each cup's three intermissions at one stride apart (88 and 64 bytes in 1.8.0), found by a scan of the
// game's memory on a thread of its own (classic_poll). Steam Deck, 15/11/2026: with the setting on at start
// (classic4) and switched on, off and on again during the Mushroom Cup's first race (classiclive2), the second race is
// Crown City for 3 laps; switched in the archive only after start (classiclive1), it stays the intermission.
//
// Classic VS (MKW_CLASSIC_VS, Hacks tab): a VS race after the first, whatever the Courses rule (Tour, Choose,
// Random), drives the route from the course before to the next one: an intermission from that course's area's
// CourseEx list (its routes to its neighbours, 202 in 1.8.0). Classic VS puts in each route's place its destination's
// course, as for the cups, so every VS race is laps on one course. The loading screen then shows the next course
// without the line of a route. Steam Deck, 16/11/2026, the archive changed in an overlay (romfs_patches): VS races
// 2 and 3 on courses for 3 laps, no errors.
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/mman.h>
#endif

#include "cfg.h"
#include "fs.h"
#include "host.h"
#include "platform/platform.h"

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t u24(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16; }


// Member i of a SARC (tools/shaders/extract.py sarc_members): its name, offset into s and length. 0 past the last
// member or when the archive is malformed.
static int sarc_member(const uint8_t *s, size_t n, uint32_t i, const char **name, size_t *off, size_t *len) {
    if (n < 0x20 || memcmp(s, "SARC", 4)) return 0;
    uint32_t header = u16(s + 4), data_start = u32(s + 12);
    if (header + 12 > n) return 0;
    uint32_t count = u16(s + header + 6), names = header + 12 + count * 16 + 8;
    if (i >= count || header + 12 + (i + 1) * 16 > n) return 0;
    const uint8_t *e = s + header + 12 + i * 16;
    uint32_t attr = u32(e + 4), lo = u32(e + 8), hi = u32(e + 12), at = names + (attr & 0xffffff) * 4;
    if (at >= n || !memchr(s + at, 0, n - at) || lo > hi || (uint64_t)data_start + hi > n) return 0;
    *name = (const char *)s + at;
    *off = data_start + lo;
    *len = hi - lo;
    return 1;
}

// The member's bytes in a SARC: offset into s, 0 when absent.
static size_t sarc_find(const uint8_t *s, size_t n, const char *name, size_t *len) {
    const char *m;
    size_t off;
    for (uint32_t i = 0; sarc_member(s, n, i, &m, &off, len); i++)
        if (!strcmp(m, name)) return off;
    return 0;
}

// MurmurHash3_x86_32 by Austin Appleby (public domain, smhasher MurmurHash3.cpp), seed 0.
static uint32_t murmur3(const char *key, size_t len) {
    const uint8_t *p = (const uint8_t *)key;
    uint32_t h = 0, k;
    const uint32_t c1 = 0xcc9e2d51, c2 = 0x1b873593;
    size_t i = 0;
    for (; i + 4 <= len; i += 4) {
        k = u32(p + i) * c1;
        k = (k << 15 | k >> 17) * c2;
        h ^= k;
        h = (h << 13 | h >> 19) * 5 + 0xe6546b64;
    }
    k = 0;
    switch (len & 3) {
        case 3: k ^= (uint32_t)p[i + 2] << 16; /* fall through */
        case 2: k ^= (uint32_t)p[i + 1] << 8; /* fall through */
        case 1: k ^= p[i]; k *= c1; k = (k << 15 | k >> 17) * c2; h ^= k;
    }
    h ^= (uint32_t)len;
    h ^= h >> 16, h *= 0x85ebca6b, h ^= h >> 13, h *= 0xc2b2ae35, h ^= h >> 16;
    return h;
}

// The 8-byte entry named key in the BYML dictionary at node of b (n bytes), NULL when absent.
static uint8_t *byml_get(uint8_t *b, size_t n, uint32_t node, const char *key) {
    if (n < 16 || node + 4 > n || b[node] != 0xC1) return NULL;
    uint32_t keys = u32(b + 4), nkeys = keys + 4 <= n ? u24(b + keys + 1) : 0;
    uint32_t count = u24(b + node + 1);
    for (uint32_t i = 0; i < count && node + 4 + 8 * (i + 1) <= n; i++) {
        uint8_t *e = b + node + 4 + 8 * i;
        uint32_t ki = u24(e);
        if (ki >= nkeys || keys + 4 + 4 * (ki + 1) > n) continue;
        uint32_t at = keys + u32(b + keys + 4 + 4 * ki);
        if (at < n && !strncmp((const char *)b + at, key, n - at)) return e;
    }
    return NULL;
}

// The string of a BYML string entry (type 0xA0), NULL otherwise.
static const char *byml_str(const uint8_t *b, size_t n, const uint8_t *e) {
    if (!e || e[3] != 0xA0) return NULL;
    uint32_t strs = u32(b + 8), i = u32(e + 4);
    if (!strs || strs + 4 > n || i >= u24(b + strs + 1) || strs + 4 + 4 * (i + 1) > n) return NULL;
    uint32_t at = strs + u32(b + strs + 4 + 4 * i);
    return at < n && memchr(b + at, 0, n - at) ? (const char *)b + at : NULL;
}

// Element i of the BYML array that entry e points to: its type, and its 4-byte value's offset in b; 0 when absent.
static uint32_t byml_elem(const uint8_t *b, size_t n, const uint8_t *e, uint32_t i, uint8_t *type) {
    if (!e || e[3] != 0xC0) return 0;
    uint32_t arr = u32(e + 4);
    if (arr + 4 > n || b[arr] != 0xC0 || i >= u24(b + arr + 1)) return 0;
    uint32_t count = u24(b + arr + 1), at = arr + 4 + ((count + 3) & ~3u) + 4 * i;
    if (arr + 4 + count > n || at + 4 > n) return 0;
    *type = b[arr + 4 + i];
    return at;
}

// The file name of a member without its folder and from its first dot on: "Kinoko" of
// "Gyml/GrandprixCupSettingGParam/Kinoko.game__GrandprixCupSettingGParam.bgyml".
static size_t stem(const char *path, const char **out) {
    const char *s = strrchr(path, '/');
    s = s ? s + 1 : path;
    *out = s;
    return strcspn(s, ".");
}

typedef struct { uint32_t id, dest; int inter; char name[64]; } ClassicCourse;
typedef struct { uint32_t key, course; } ClassicArea;

#define CLASSIC_COURSES 1024   // 1.8.0 has 310 CourseGParam and 30 CourseAreaGParam
#define CLASSIC_CUPS 32        // 8 in GrandprixSettingGParam/Grandprix

static const ClassicCourse *classic_course(const ClassicCourse *c, int n, uint32_t id) {
    for (int i = 0; i < n; i++)
        if (c[i].id == id) return &c[i];
    return NULL;
}

// One CourseId that Classic changes: the game's value, Classic's, its group (a cup's intermissions or an area's
// routes, in the archive's order: they lie at one stride in the game's memory too) and which setting it follows.
enum { CLASSIC_GP, CLASSIC_VS };
typedef struct { uint32_t orig, classic; uint16_t group; uint8_t kind; } ClassicSlot;
#define CLASSIC_SLOTS 512   // 1.8.0: 24 in the cups, 202 routes in the areas

// The destination's course of intermission id, NULL when it has none.
static const ClassicCourse *classic_dest(const ClassicCourse *c, int nc, const ClassicArea *a, int na, uint32_t id) {
    const ClassicCourse *from = classic_course(c, nc, id), *to = NULL;
    if (!from || !from->inter) return NULL;
    for (int k = 0; k < na && !to; k++)
        if (a[k].key == from->dest) to = classic_course(c, nc, a[k].course);
    return to && !to->inter ? to : NULL;
}

// The intermissions in s (the decompressed archive, n bytes) and their destinations' courses, into rec
// (CLASSIC_SLOTS entries): their number. Those of the Grand Prix cups (MKW_CLASSIC_GP) and every area's routes to its
// neighbours (CourseAreaGParam CourseEx, MKW_CLASSIC_VS: a VS race after the first drives the route from the course
// before to the one chosen). write[kind]: those courses go into s too.
static int classic_patch(uint8_t *s, size_t n, ClassicSlot *rec, const int write[2]) {
    ClassicCourse *course = calloc(CLASSIC_COURSES, sizeof *course);
    ClassicArea *area = calloc(CLASSIC_COURSES * 2, sizeof *area);
    char cups[CLASSIC_CUPS][96];
    int ncourse = 0, narea = 0, ncups = 0, changed = 0, group = 0;
    if (!course || !area) { free(course); free(area); return 0; }
    const char *name, *st;
    size_t off, len;
    for (uint32_t i = 0; sarc_member(s, n, i, &name, &off, &len); i++) {
        uint8_t *b = s + off;
        if (len < 16 || memcmp(b, "YB", 2)) continue;
        uint32_t root = u32(b + 12);
        size_t sl = stem(name, &st);
        if (!strncmp(name, "Gyml/CourseGParam/", 18) && ncourse < CLASSIC_COURSES && sl < sizeof course->name) {
            ClassicCourse *c = &course[ncourse++];
            memcpy(c->name, st, sl);
            c->name[sl] = 0;
            c->id = murmur3(st, sl);
            c->inter = !strncmp(st, "InterStage", 10);
            uint8_t *d = byml_get(b, len, root, "DestStageIdValue");
            c->dest = d && (d[3] == 0xD3 || d[3] == 0xD1) ? u32(d + 4) : 0;
        } else if (!strncmp(name, "Gyml/CourseAreaGParam/", 22) && narea + 2 <= CLASSIC_COURSES * 2) {
            uint8_t t = 0;
            uint32_t at = byml_elem(b, len, byml_get(b, len, root, "NormalCourse"), 0, &t);
            if (!at || (t != 0xD3 && t != 0xD1)) continue;
            const char *type = byml_str(b, len, byml_get(b, len, root, "StageType"));
            uint32_t keys[2] = {type ? murmur3(type, strlen(type)) : 0, murmur3(st, sl)};
            for (int k = 0; k < 2; k++) {   // the first area that names a key keeps it
                int seen = !keys[k];
                for (int j = 0; j < narea && !seen; j++) seen = area[j].key == keys[k];
                if (!seen) area[narea++] = (ClassicArea){keys[k], u32(b + at)};
            }
        } else if (!strncmp(name, "Gyml/GrandprixSettingGParam/Grandprix.", 38)) {
            uint8_t *list = byml_get(b, len, root, "Cups"), t;
            for (uint32_t j = 0; ncups < CLASSIC_CUPS; j++) {
                uint32_t at = byml_elem(b, len, list, j, &t);
                if (!at) break;
                uint8_t e[8] = {0, 0, 0, t};   // a string element as a dictionary entry, for byml_str
                memcpy(e + 4, b + at, 4);
                const char *cup = byml_str(b, len, e);
                size_t cl = cup ? stem(cup, &cup) : 0;
                if (cl && cl < sizeof cups[0]) memcpy(cups[ncups], cup, cl), cups[ncups++][cl] = 0;
            }
        }
    }
    for (uint32_t i = 0; sarc_member(s, n, i, &name, &off, &len); i++) {
        uint8_t *b = s + off;
        if (strncmp(name, "Gyml/GrandprixCupSettingGParam/", 31) || len < 16 || memcmp(b, "YB", 2)) continue;
        size_t sl = stem(name, &st);
        int in_gp = 0;
        for (int j = 0; j < ncups && !in_gp; j++) in_gp = strlen(cups[j]) == sl && !strncmp(cups[j], st, sl);
        if (!in_gp) continue;
        uint8_t *list = byml_get(b, len, u32(b + 12), "CourseList"), t;
        for (uint32_t j = 0;; j++) {
            uint32_t at = byml_elem(b, len, list, j, &t);
            if (!at) break;
            uint8_t *id = t == 0xC1 ? byml_get(b, len, u32(b + at), "CourseId") : NULL;
            if (!id || (id[3] != 0xD3 && id[3] != 0xD1)) continue;
            const ClassicCourse *from = classic_course(course, ncourse, u32(id + 4));
            if (!from || !from->inter) continue;
            const ClassicCourse *to = classic_dest(course, ncourse, area, narea, from->id);
            if (!to) {
                rt_log("classic grand prix: %.*s: no course for %s, kept", (int)sl, st, from->name);
                continue;
            }
            if (changed >= CLASSIC_SLOTS) break;
            rt_log("classic grand prix: %.*s race %u: %s instead of %s", (int)sl, st, j + 1, to->name, from->name);
            rec[changed++] = (ClassicSlot){from->id, to->id, (uint16_t)group, CLASSIC_GP};
            if (write[CLASSIC_GP]) memcpy(id + 4, &to->id, 4);   // little-endian, as the BYML
        }
        group++;
    }
    int routes = 0;
    for (uint32_t i = 0; sarc_member(s, n, i, &name, &off, &len); i++) {
        uint8_t *b = s + off;
        if (strncmp(name, "Gyml/CourseAreaGParam/", 22) || len < 16 || memcmp(b, "YB", 2)) continue;
        uint8_t *list = byml_get(b, len, u32(b + 12), "CourseEx"), t;
        for (uint32_t j = 0; changed < CLASSIC_SLOTS; j++) {
            uint32_t at = byml_elem(b, len, list, j, &t);
            if (!at) break;
            const ClassicCourse *to = t == 0xD3 || t == 0xD1 ? classic_dest(course, ncourse, area, narea, u32(b + at)) : NULL;
            if (!to) continue;
            rec[changed++] = (ClassicSlot){u32(b + at), to->id, (uint16_t)group, CLASSIC_VS};
            if (write[CLASSIC_VS]) memcpy(b + at, &to->id, 4);
            routes++;
        }
        group++;
    }
    rt_log("classic vs: %d routes between the courses", routes);
    if (!ncups) rt_log("classic grand prix: no cups in GrandprixSettingGParam/Grandprix: the game's own");
    free(course);
    free(area);
    return changed;
}

// ---- live: the game's own copies of the cups and the routes

#define LIVE_STRIDE 1024   // the largest stride between a group's CourseIds that the scan tries, in bytes
#define LIVE_HITS 1024
#define LIVE_MIN 3         // a group is looked for by its first three: shorter ones are not switched live

static struct {
    ClassicSlot slot[CLASSIC_SLOTS];
    int nslot, busy;
    int on[2];     // what the game's copies hold: MKW_CLASSIC_GP, MKW_CLASSIC_VS
    int want[2];   // what the running scan writes
} g_live;

static const char *const kKey[2] = {"MKW_CLASSIC_GP", "MKW_CLASSIC_VS"};

// The number of slots in the group that starts at slot i, 0 when i does not start one of at least LIVE_MIN.
static int live_group(const ClassicSlot *sl, int n, int i) {
    if (i && sl[i - 1].group == sl[i].group) return 0;
    int k = 1;
    while (i + k < n && sl[i + k].group == sl[i].group) k++;
    return k >= LIVE_MIN ? k : 0;
}

typedef struct { uint64_t at; int first, count, stride; } LiveHit;

// Whether the count words from w at stride d (in words) hold the group's values from slot i, all the game's or all
// Classic's.
static int live_group_at(const uint32_t *w, uint64_t d, const ClassicSlot *sl, int i, int count, int classic) {
    for (int k = 0; k < count; k++)
        if (w[k * d] != (classic ? sl[i + k].classic : sl[i + k].orig)) return 0;
    return 1;
}

// The hits in the nw words of w at guest address base: a group's CourseIds at one stride.
static int live_words(const uint32_t *w, uint64_t nw, uint64_t base, const ClassicSlot *sl, int n, const uint8_t *bits,
                      LiveHit *hit, int nhit) {
    for (uint64_t k = 0; k < nw; k++) {
        uint32_t v = w[k];
        if (!(bits[(v & 0xffff) >> 3] & 1 << (v & 7))) continue;
        for (int i = 0; i < n; i++) {
            if (v != sl[i].orig && v != sl[i].classic) continue;
            int count = live_group(sl, n, i), c = v == sl[i].classic;
            if (!count) continue;
            for (uint64_t d = 1; d <= LIVE_STRIDE / 4 && k + (uint64_t)(count - 1) * d < nw; d++)
                if (live_group_at(w + k, d, sl, i, count, c) && nhit < LIVE_HITS)
                    hit[nhit++] = (LiveHit){base + k * 4, i, count, (int)d * 4};
        }
    }
    return nhit;
}

// The scan takes seconds: a candidate may have been freed or reused before it is written.
static int live_still_matches(const LiveHit *h, const ClassicSlot *sl) {
    int original = 1, classic = 1;
    for (int k = 0; k < h->count; k++) {
        uint64_t at = h->at + (uint64_t)k * (uint64_t)h->stride, end;
        int state = plat_mem_state(at, at + 4, &end);
        if ((state != PLAT_PROT_RW && state != PLAT_PROT_READ) || end < at + 4) return 0;
        uint32_t value = __atomic_load_n((const uint32_t *)(uintptr_t)at, __ATOMIC_RELAXED);
        original &= value == sl[h->first + k].orig;
        classic &= value == sl[h->first + k].classic;
    }
    return original || classic;
}

static void live_scan(void *arg) {
    (void)arg;
    int n = g_live.nslot, nhit = 0, want[2] = {g_live.want[0], g_live.want[1]};
    const ClassicSlot *sl = g_live.slot;
    static uint8_t bits[8192];   // the low 16 bits of each group's first CourseId, both values: a quick no
    static LiveHit hit[LIVE_HITS];
    memset(bits, 0, sizeof bits);
    for (int i = 0; i < n; i++)
        if (live_group(sl, n, i))
            for (int c = 0; c < 2; c++) {
                uint32_t v = c ? sl[i].classic : sl[i].orig;
                bits[(v & 0xffff) >> 3] |= (uint8_t)(1 << (v & 7));
            }
    uint64_t t0 = plat_now_ns(), scanned = 0;
    const RtGuestLayout *L = mem_layout();
    RtRange region[2] = {L->heap, L->alias};
    for (int r = 0; r < 2; r++) {
        uint64_t a = region[r].base, end = region[r].base + region[r].size;
        while (a < end) {
            uint64_t b = end;
            int st = plat_mem_state(a, end, &b);
            if (b <= a) break;
            for (uint64_t c = a; (st == PLAT_PROT_RW || st == PLAT_PROT_READ) && c < b; c += 2ull << 20) {
                uint64_t ce = c + (2ull << 20) < b ? c + (2ull << 20) : b, lo = c;
#ifndef _WIN32
                unsigned char vec[512];   // only the pages in memory: the rest of the heap was never touched
                if (mincore((void *)(uintptr_t)c, ce - c, vec)) continue;
                for (uint64_t at = c; at <= ce; at += 4096) {
                    if (at < ce && vec[(at - c) / 4096] & 1) continue;
                    if (at > lo) {
                        scanned += at - lo;
                        nhit = live_words((const uint32_t *)(uintptr_t)lo, (at - lo) / 4, lo, sl, n, bits, hit, nhit);
                    }
                    lo = at + 4096;
                }
#else
                scanned += ce - c;
                nhit = live_words((const uint32_t *)(uintptr_t)c, (ce - c) / 4, c, sl, n, bits, hit, nhit);
#endif
            }
            a = b;
        }
    }
    int applied[2] = {0, 0};
    for (int h = 0; h < nhit; h++) {
        int kind = sl[hit[h].first].kind;
        if (!live_still_matches(&hit[h], sl)) continue;
        mem_thaw(hit[h].at, (uint64_t)hit[h].stride * (uint64_t)(hit[h].count - 1) + 4);   // a frozen region takes writes
        if (!live_still_matches(&hit[h], sl)) continue;
        for (int k = 0; k < hit[h].count; k++) {
            const ClassicSlot *s = &sl[hit[h].first + k];
            __atomic_store_n((uint32_t *)(uintptr_t)(hit[h].at + (uint64_t)k * (uint64_t)hit[h].stride),
                             want[kind] ? s->classic : s->orig, __ATOMIC_RELAXED);
        }
        applied[kind]++;
    }
    rt_log("classic: grand prix %s, vs %s for future races: %d and %d places in the game's memory (%d stale skipped, "
           "%.0f MiB in %.1f s)", want[0] ? "on" : "off", want[1] ? "on" : "off", applied[0], applied[1],
           nhit - applied[0] - applied[1], scanned / 1048576.0, (plat_now_ns() - t0) / 1e9);
    if (nhit == LIVE_HITS) rt_log("classic: %d places found, more were not looked at", nhit);
    __atomic_store_n(&g_live.busy, 0, __ATOMIC_RELEASE);
}

void classic_seen(void *p, size_t n) {
    uint8_t *s = p;
    size_t len;
    if (n < 0x20 || memcmp(s, "SARC", 4) || g_live.nslot) return;
    if (!sarc_find(s, n, "Gyml/GrandprixSettingGParam/Grandprix.game__GrandprixSettingGParam.bgyml", &len)) return;
    int on[2] = {cfg_bool(kKey[0]), cfg_bool(kKey[1])};
    int k = classic_patch(s, n, g_live.slot, on);
    g_live.on[0] = on[0], g_live.on[1] = on[1];
    __atomic_store_n(&g_live.nslot, k, __ATOMIC_RELEASE);
    // none when a mod already made them classic
    rt_log("classic: %d intermissions in the cups and routes, grand prix %s, vs %s (the Hacks tab switches them)", k,
           on[0] ? "on" : "off", on[1] ? "on" : "off");
}

void classic_poll(void) {
    static unsigned seen;
    unsigned v = cfg_version();
    if (v == seen || !__atomic_load_n(&g_live.nslot, __ATOMIC_ACQUIRE) || __atomic_load_n(&g_live.busy, __ATOMIC_ACQUIRE))
        return;   // a change during a scan: on the frame after it
    seen = v;
    int on[2] = {cfg_bool(kKey[0]), cfg_bool(kKey[1])};
    if (on[0] == g_live.on[0] && on[1] == g_live.on[1]) return;
    int was[2] = {g_live.on[0], g_live.on[1]};
    g_live.on[0] = g_live.want[0] = on[0], g_live.on[1] = g_live.want[1] = on[1];
    __atomic_store_n(&g_live.busy, 1, __ATOMIC_RELEASE);
    if (!plat_thread_spawn(live_scan, NULL, "classic", 64 << 10)) {
        rt_log("classic: no thread for the switch");
        g_live.on[0] = was[0], g_live.on[1] = was[1], g_live.busy = 0;
    }
}
