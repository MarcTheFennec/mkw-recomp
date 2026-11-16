// The game's own English name of a course for the Discord status ("MooMooFarm" -> "Moo Moo Meadows"), read from the
// player's RomFS: Mals/USen/MsgMain.msarc, a SARC of MSBT files (format as tools/i18n/game_texts.py reads it), where
// CourseArea.msbt names each course "Name_<internal>" or "Name_<internal>_00" and BattleCourse.msbt names the battle
// courses by number. Nothing of the game in this code: the names come from the player's files at run time, once, on
// the presence thread.
//
// The battle courses' own internal names (Env/BalloonBattle_<internal>_...) are not in any text: four differ from
// their race course's, matched from their thumbnails' file names (C_BattleL_<internal>_00, Super Mario Wiki's
// "Internal names" tables, 16/11/2026); the others (FirstCircuit, Central, MooMooFarm, LagoonCity) are named like their
// race course.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "course_names.h"
#include "fs.h"
#include "platform/platform.h"
#include "utf16.h"

#define MAX_NAMES 96

typedef struct {
    char label[48];   // "CourseArea/Name_MooMooFarm_00", "BattleCourse/1705"
    char text[64];    // UTF-8
} Name;

static Name names[MAX_NAMES];
static int n_names;

static const struct { const char *internal, *label; } battles[] = {
    {"Volcano", "BattleCourse/1705"},   // Big Donut
    {"Desert", "BattleCourse/1702"},    // Chain Chomp Desert
    {"Jungle", "BattleCourse/1704"},    // Dino Dino Jungle
    {"Ice", "BattleCourse/1708"},       // DK Pass
};

static uint32_t rd16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

// One text of TXT2 (UTF-16LE up to its NUL) as UTF-8, its control tags (0x0E group type size args, 0x0F group type)
// dropped.
static void text_utf8(const uint8_t *p, size_t bytes, char *out, size_t cap) {
    uint16_t units[64];
    size_t n = 0;
    for (size_t i = 0; i + 1 < bytes && n + 1 < 64;) {
        uint16_t u = (uint16_t)rd16(p + i);
        if (!u) break;
        if (u == 0x0E && i + 8 <= bytes) i += 8 + rd16(p + i + 6);
        else if (u == 0x0F) i += 6;
        else units[n++] = u, i += 2;
    }
    units[n] = 0;
    utf16_to_utf8(units, n, out, cap);
}

// The (label, text) pairs of one MSBT into names[], each label prefixed "<msbt>/".
static void read_msbt(const uint8_t *d, size_t size, const char *msbt) {
    if (size < 0x20 || memcmp(d, "MsgStdBn", 8) || d[8] != 0xff || d[9] != 0xfe || d[0x0c] != 1) return;
    const uint8_t *lbl = NULL, *txt = NULL;
    size_t lbl_n = 0, txt_n = 0;
    for (size_t off = 0x20, k = 0, count = rd16(d + 0x0e); k < count && off + 16 <= size; k++) {
        size_t len = rd32(d + off + 4);
        if (off + 16 + len > size) return;
        if (!memcmp(d + off, "LBL1", 4)) lbl = d + off + 16, lbl_n = len;
        if (!memcmp(d + off, "TXT2", 4)) txt = d + off + 16, txt_n = len;
        off += 16 + ((len + 15) & ~(size_t)15);
    }
    if (!lbl || !txt || lbl_n < 4 || txt_n < 4) return;
    uint32_t n_txt = rd32(txt);
    if (4 + (size_t)n_txt * 4 > txt_n) return;
    for (uint32_t g = 0, groups = rd32(lbl); g < groups && 4 + g * 8 + 8 <= lbl_n; g++) {
        uint32_t count = rd32(lbl + 4 + g * 8);
        size_t q = rd32(lbl + 8 + g * 8);
        for (uint32_t j = 0; j < count && q < lbl_n && n_names < MAX_NAMES; j++) {
            size_t ln = lbl[q];
            if (q + 1 + ln + 4 > lbl_n) return;
            uint32_t idx = rd32(lbl + q + 1 + ln);
            if (idx < n_txt) {
                size_t from = rd32(txt + 4 + idx * 4), to = idx + 1 < n_txt ? rd32(txt + 8 + idx * 4) : txt_n;
                Name *nm = &names[n_names];
                if (from < to && to <= txt_n && snprintf(nm->label, sizeof nm->label, "%s/%.*s", msbt, (int)ln,
                                                         (const char *)lbl + q + 1) < (int)sizeof nm->label) {
                    text_utf8(txt + from, to - from, nm->text, sizeof nm->text);
                    if (*nm->text) n_names++;
                }
            }
            q += 1 + ln + 4;
        }
    }
}

// The SARC's CourseArea.msbt and BattleCourse.msbt.
static void read_sarc(const uint8_t *d, size_t size) {
    if (size < 0x14 || memcmp(d, "SARC", 4) || d[6] != 0xff || d[7] != 0xfe) return;
    size_t sfat = rd16(d + 4), data = rd32(d + 12);
    if (sfat + 12 > size || memcmp(d + sfat, "SFAT", 4)) return;
    size_t count = rd16(d + sfat + 6), nodes = sfat + rd16(d + sfat + 4), sfnt = nodes + count * 16;
    if (sfnt + 8 > size || memcmp(d + sfnt, "SFNT", 4)) return;
    for (size_t i = 0; i < count; i++) {
        const uint8_t *node = d + nodes + i * 16;
        uint32_t attr = rd32(node + 4), start = rd32(node + 8), end = rd32(node + 12);
        if (!(attr & 0x01000000)) continue;
        size_t name = sfnt + 8 + (attr & 0xffff) * 4;
        if (name >= size || !memchr(d + name, 0, size - name) || data + end > size || start > end) continue;
        const char *fn = (const char *)d + name;
        if (!strcmp(fn, "CourseArea.msbt")) read_msbt(d + data + start, end - start, "CourseArea");
        else if (!strcmp(fn, "BattleCourse.msbt")) read_msbt(d + data + start, end - start, "BattleCourse");
    }
}

static void load(void *arg) {
    (void)arg;
    static const char *const folders[] = {"Mals/USen/MsgMain.msarc", "Mals/EUen/MsgMain.msarc"};
    for (size_t i = 0; i < sizeof folders / sizeof *folders && !n_names; i++) {
        uint64_t size = 0;
        uint8_t *d = romfs_read_whole(folders[i], &size);
        if (d) read_sarc(d, (size_t)size);
        free(d);
    }
}

static const char *find(const char *label) {
    for (int i = 0; i < n_names; i++)
        if (!strcmp(names[i].label, label)) return names[i].text;
    return NULL;
}

int course_name(const char *internal, int battle, char *out, size_t cap) {
    static PlatOnce once = PLAT_ONCE_INIT;
    if (!romfs_file_count()) return 0;   // no RomFS yet (or --probe-discord without the game): try again next time
    plat_once(&once, load, NULL);
    const char *name = NULL;
    char label[96];
    for (size_t i = 0; battle && !name && i < sizeof battles / sizeof *battles; i++)
        if (!strcmp(internal, battles[i].internal)) name = find(battles[i].label);
    if (!name && snprintf(label, sizeof label, "CourseArea/Name_%s_00", internal) < (int)sizeof label) name = find(label);
    if (!name && snprintf(label, sizeof label, "CourseArea/Name_%s", internal) < (int)sizeof label) name = find(label);
    if (!name) return 0;
    snprintf(out, cap, "%s", name);
    return 1;
}
