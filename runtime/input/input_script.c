// Scripted and recorded input (input.c merges it): MKW_INPUT_SCRIPT and MKW_QUICK_RACE presses, MKW_CTL injections and
// MKW_INPUT_RECORD. Split from input.c; every function here runs on the input thread or ctl.c's, under script_lock.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "hacks.h"
#include "host.h"
#include "input.h"
#include "input_map.h"
#include "input_script.h"
#include "nvn_api.h"
#include "vk.h"
#include "platform/platform.h"

// MKW_INPUT_SCRIPT=<file>: lines "<frame> <button> [hold]" or "<frame> LSTICK|RSTICK <x> <y> [hold]", ORed into the real input.
// <frame> and its hold are 60 Hz ticks of game time since the first present (frame_step_ticks: presents at 60 fps), so
// the title menus get the presses at the same game time at any frame limit. A line "@<ms> ..." is timed instead: <ms>
// and its hold are milliseconds of the world clock (frame_step_world_us, game time since the loaded save's first
// frame), so it lands at the same game time at any frame rate and load length. A timed event shorter than a frame is
// applied once. In a script with timed lines (a recording) the untimed ones belong before the world clock: they stop
// when it starts, as the load ends at another tick in each run.
// at, hold: ticks, or microseconds of the world clock when timed.
// A line "<node>+<ticks> ..." waits for a phase of the game instead (MKW_SEQ_WATCH=1, seqwatch.c): it lands <ticks>
// after the node's first start, so a press in a menu does not depend on how long the menus before it took (turbo, loads).
// A line that starts with "p<N> " (N 1-8) presses for player N (input_poll_player); without it, for player 1.
typedef struct {
    uint64_t at, hold;
    int bit, stick, timed, player;
    int32_t x, y;
    int logged;
    char text[48], node[40];   // node: the phase it waits for, "" when none or already started
} ScriptEv;
static const struct { const char *name; int bit; } script_buttons[] = {
    {"A", B_A}, {"B", B_B}, {"X", B_X}, {"Y", B_Y}, {"LCLICK", B_STICK_L}, {"RCLICK", B_STICK_R}, {"L", B_L}, {"R", B_R},
    {"ZL", B_ZL}, {"ZR", B_ZR}, {"PLUS", B_PLUS}, {"MINUS", B_MINUS}, {"LEFT", B_LEFT}, {"UP", B_UP}, {"RIGHT", B_RIGHT}, {"DOWN", B_DOWN},
};
static ScriptEv *script_ev;
static int script_n, script_state;   // 0 not loaded, 1 loaded
static PlatMutex script_lock = PLAT_MUTEX_INIT;   // script_ev: input thread and ctl.c
static int script_mixed;             // the script has timed lines: its untimed ones end where the world clock starts

// One script line into *e: 1 when it is an event, 0 for a blank or comment line, -1 (logged) when malformed.
static int script_parse(char *line, int ln, ScriptEv *out) {
    char *h = strchr(line, '#');
    if (h) *h = 0;
    int player = 0;
    if (line[0] == 'p' && line[1] >= '1' && line[1] <= '0' + INPUT_MAX_PLAYERS && line[2] == ' ')
        player = line[1] - '1', line += 3;
    char name[32], node[40] = "";
    double at;
    int off = 0, timed = line[0] == '@', skip = timed;
    if ((line[0] >= 'A' && line[0] <= 'Z') && sscanf(line, "%39[A-Za-z0-9_]+%n", node, &skip) == 1 && skip) timed = 0;
    else node[0] = 0, skip = timed;
    if (sscanf(line + skip, "%lf %31s%n", &at, name, &off) < 2 || at < 0) return 0;
    off += skip;
    double unit = timed ? 1000.0 : 1.0;   // ms to us, or presents
    ScriptEv e = {.at = (uint64_t)(at * unit + 0.5), .hold = timed ? 66667 : 4, .bit = -1, .timed = timed, .player = player};
    char who[4] = "";
    if (player) snprintf(who, sizeof who, "p%d ", player + 1);
    long a, b;
    double c;
    if (!strcmp(name, "LSTICK") || !strcmp(name, "RSTICK")) {
        int n = sscanf(line + off, "%ld %ld %lf", &a, &b, &c);
        if (n < 2) { rt_log("input: script line %d: %s needs x y [hold]", ln, name); return -1; }
        e.stick = name[0] == 'L' ? 1 : 2; e.x = (int32_t)a; e.y = (int32_t)b;
        if (n >= 3 && c > 0) e.hold = (uint64_t)(c * unit + 0.5);
        snprintf(e.text, sizeof e.text, "%s%s %d %d", who, name, e.x, e.y);
    } else {
        for (size_t i = 0; i < sizeof script_buttons / sizeof *script_buttons; i++)
            if (!strcmp(name, script_buttons[i].name)) e.bit = script_buttons[i].bit;
        if (e.bit < 0) { rt_log("input: script line %d: unknown button %s", ln, name); return -1; }
        if (sscanf(line + off, "%lf", &c) == 1 && c > 0) e.hold = (uint64_t)(c * unit + 0.5);
        snprintf(e.text, sizeof e.text, "%s%s", who, name);
    }
    snprintf(e.node, sizeof e.node, "%s", node);
    *out = e;
    return 1;
}

static void script_load(void);

// seqwatch.c: a phase started; the script lines waiting for it count their ticks from now.
void input_script_phase(const char *node) {
    plat_mutex_lock(&script_lock);
    if (!script_state) script_load();
    uint64_t now = frame_step_ticks() + 1;
    for (int i = 0; i < script_n; i++)
        if (script_ev[i].node[0] && !strcmp(script_ev[i].node, node)) script_ev[i].at += now, script_ev[i].node[0] = 0;
    plat_mutex_unlock(&script_lock);
}

static void script_add(const ScriptEv *e) {
    static int cap;
    if (script_n == cap) {
        cap = cap ? cap * 2 : 32;
        if (!(script_ev = realloc(script_ev, (size_t)cap * sizeof *script_ev))) rt_fatal("input: out of memory (script)");
    }
    script_ev[script_n++] = *e;
}

// MKW_QUICK_RACE (Hacks tab): the menus pressed through by phase (seqwatch.c turns the watch on
// for it). tt: a time trial, Mario, standard kart, the course the map offers (the title's; fixed with MKW_SEED, tt6a/b),
// alone (tt2: race 29,6 s after boot); gp: a Grand Prix, the first entry everywhere (the character, kart and cup the
// menus offer: the save's last ones), against 23 rivals; free: free roam, the main menu's Plus (Tab on the keyboard).
static const char *const quick_tt[] = {
    "TitleWait+40 A 6",       "TitleMenuMain+40 A 6",     "TitleMenuMain+160 DOWN 6", "TitleMenuMain+190 DOWN 6",
    "TitleMenuMain+230 A 6",  "PartsSelectEx+60 A 6",     "PartsSelectEx+180 A 6",    "VSStageSelect+60 A 6",
    "MenuTimeAttackMain+60 A 6", "SimpleConfirm+40 A 6",
};
// gp: GrandprixListup takes the cup and then its start; a third A there lands on SimpleConfirm or later when the second
// already went on. IntoRace2 starts the course's flyover, which A skips (a press, not a hold: four of them, 3 s apart). The race
// needs no A: Smart Steering and Auto-Accelerate (the game's own options, kept in the save) drive the kart, as they
// cannot in a time trial; without them A held after the flyover keeps the kart moving, into the walls.
// Steam Deck, 14/11/2026 (1.8.0): A pressed every 1.5 s reached the race (gp1); these are the presses that landed.
static const char *const quick_gp[] = {
    "TitleWait+40 A 6",         "TitleMenuMain+40 A 6",     "TitleMenuMain+230 A 6",   "PartsSelectEx+60 A 6",
    "PartsSelectEx+180 A 6",    "GrandprixListup+60 A 6",   "GrandprixListup+150 A 6", "GrandprixListup+240 A 6",
    "SimpleConfirm+40 A 6",     "IntoRace2+120 A 6",        "IntoRace2+300 A 6",       "IntoRace2+480 A 6",
    "IntoRace2+660 A 6",        "IntoRace2+800 A 4320000",
};
static const char *const quick_free[] = {"TitleWait+40 A 6", "TitleMenuMain+60 PLUS 6"};

static void script_load_quick(const char *mode) {
    int free_roam = !strcmp(mode, "free"), gp = !strcmp(mode, "gp");
    const char *const *lines = free_roam ? quick_free : gp ? quick_gp : quick_tt;
    int n = free_roam ? (int)(sizeof quick_free / sizeof *quick_free)
            : gp      ? (int)(sizeof quick_gp / sizeof *quick_gp)
                      : (int)(sizeof quick_tt / sizeof *quick_tt);
    for (int i = 0; i < n; i++) {
        char line[64];
        ScriptEv e;
        snprintf(line, sizeof line, "%s", lines[i]);
        if (script_parse(line, i + 1, &e) > 0) script_add(&e);
    }
    rt_log("input: MKW_QUICK_RACE=%s, %d presses by phase", mode, script_n);
}

static void script_load(void) {
    script_state = 1;
    const char *path = cfg_get("MKW_INPUT_SCRIPT"), *quick = cfg_value("MKW_QUICK_RACE");
    if ((!path || !*path) && strcmp(quick, "off")) {
        script_load_quick(quick);
        return;
    }
    if (!path || !*path) return;
    FILE *f = fopen(path, "r");
    if (!f) { rt_log("input: cannot open MKW_INPUT_SCRIPT %s", path); return; }
    char line[256];
    int ln = 0;
    ScriptEv e;
    while (fgets(line, sizeof line, f)) {
        if (script_parse(line, ++ln, &e) <= 0) continue;
        script_add(&e);
        script_mixed |= e.timed;
    }
    fclose(f);
    rt_log("input: script %s, %d events", path, script_n);
}

static void script_apply_locked(uint64_t *b, int32_t sl[2], int32_t sr[2], int player);

// MKW_CTL (ctl.c): a line "<ticks from now> <button> [hold]" or "... LSTICK|RSTICK x y [hold]" applied from the next
// tick, beside the script's own events.
int input_script_inject(const char *text) {
    char line[256];
    snprintf(line, sizeof line, "%s", text);
    ScriptEv e;
    if (script_parse(line, 0, &e) <= 0 || e.timed) return 0;
    plat_mutex_lock(&script_lock);
    if (!script_state) script_load();
    e.at += frame_step_ticks() + 1;
    script_add(&e);
    plat_mutex_unlock(&script_lock);
    return 1;
}

void script_apply(uint64_t *b, int32_t sl[2], int32_t sr[2], int player) {
    plat_mutex_lock(&script_lock);
    if (!script_state) script_load();
    script_apply_locked(b, sl, sr, player);
    plat_mutex_unlock(&script_lock);
}

static void script_apply_locked(uint64_t *b, int32_t sl[2], int32_t sr[2], int player) {
    if (!script_n || seqwatch_hold()) return;   // MKW_SEQ_HOLD: e.g. no presses during the cup movie
    uint64_t fr = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED), tick = frame_step_ticks();
    int64_t w = frame_step_world_us();
    for (int i = 0; i < script_n; i++) {
        ScriptEv *e = &script_ev[i];
        if (e->player != player || e->node[0] || (e->timed ? w < 0 : script_mixed && w >= 0)) continue;
        uint64_t now = e->timed ? (uint64_t)w : tick;
        if (now < e->at || (now >= e->at + e->hold && (e->logged || !e->timed))) continue;
        if (!e->logged) {
            e->logged = 1;
            if (e->timed) rt_log("input: script frame %llu at %.1f ms: %s", (unsigned long long)fr, w / 1000.0, e->text);
            else rt_log("input: script frame %llu tick %llu: %s", (unsigned long long)fr, (unsigned long long)tick, e->text);
        }
        if (e->stick) { int32_t *s = e->stick == 1 ? sl : sr; s[0] = e->x; s[1] = e->y; }
        else *b |= 1ull << e->bit;
    }
}

// MKW_INPUT_RECORD=<file>: writes the merged input as MKW_INPUT_SCRIPT lines, one per button press and per stick
// segment, so a played session replays as a script. A stick segment ends when an axis moves by more than REC_STICK_STEP.
// An event that begins on the world clock is written timed ("@<ms>", hold in ms), one before it in ticks
// (frame_step_ticks). When the world clock starts, held buttons and sticks are cut there and go on as timed events.
enum { REC_STICK_STEP = 2048 };
static FILE *rec;
static int rec_state;                // 0 not opened, 1 recording, -1 off
static uint64_t rec_down[16];        // tick + 1 at which each button went down, 0 while up
static int64_t rec_down_w[16];       // the world clock then, -1 before it started
static uint64_t rec_stick_from[2];   // tick + 1 at which the current non-zero stick segment began
static int64_t rec_stick_w[2];
static int32_t rec_stick[2][2];
static int64_t rec_last_w = -1;      // the world clock at the previous apply

// "<tick> <what> <hold>" or "@<ms> <what> <hold ms>": from tick from and world clock from_w to now.
static void rec_line(uint64_t from, int64_t from_w, uint64_t fr, int64_t w, const char *what) {
    if (from_w >= 0 && w >= from_w)
        fprintf(rec, "@%.1f %s %.1f\n", from_w / 1000.0, what, (w > from_w ? w - from_w : 1000) / 1000.0);
    else
        fprintf(rec, "%llu %s %llu\n", (unsigned long long)from, what, (unsigned long long)(fr > from ? fr - from : 1));
}

static void rec_close_stick(int s, uint64_t fr, int64_t w) {
    if (!rec_stick_from[s]) return;
    char what[48];
    snprintf(what, sizeof what, "%s %d %d", s ? "RSTICK" : "LSTICK", rec_stick[s][0], rec_stick[s][1]);
    rec_line(rec_stick_from[s] - 1, rec_stick_w[s], fr, w, what);
    rec_stick_from[s] = 0;
}

static void rec_flush_all(void) {
    if (rec_state != 1) return;
    uint64_t fr = frame_step_ticks() + 1;
    int64_t w = frame_step_world_us();
    for (int i = 0; i < 16; i++)
        if (rec_down[i]) { rec_line(rec_down[i] - 1, rec_down_w[i], fr, w, script_buttons[i].name); rec_down[i] = 0; }
    rec_close_stick(0, fr, w);
    rec_close_stick(1, fr, w);
    fflush(rec);
}

void rec_apply(uint64_t b, const int32_t sl[2], const int32_t sr[2]) {
    if (!rec_state) {
        const char *path = cfg_get("MKW_INPUT_RECORD");   // no default: not recording when unset
        rec = path && *path ? fopen(path, "w") : NULL;
        rec_state = rec ? 1 : -1;
        if (path && *path && !rec) rt_log("input: cannot open MKW_INPUT_RECORD %s", path);
        if (rec) { fprintf(rec, "# Recorded input (MKW_INPUT_RECORD); frame = 60 Hz ticks of game time, @ms = world clock.\n"); atexit(rec_flush_all);
                   rt_log("input: recording to %s", path); }
    }
    if (rec_state != 1) return;
    uint64_t fr = frame_step_ticks();
    int64_t w = frame_step_world_us();
    int wrote = 0;
    if (w >= 0 && rec_last_w < 0) {   // the world clock starts: what is held goes on as timed events from @0
        for (int i = 0; i < 16; i++)
            if (rec_down[i]) {
                rec_line(rec_down[i] - 1, -1, fr, -1, script_buttons[i].name);
                rec_down[i] = fr + 1, rec_down_w[i] = w;
                wrote = 1;
            }
        for (int s = 0; s < 2; s++)
            if (rec_stick_from[s]) {
                rec_close_stick(s, fr, -1);
                rec_stick_from[s] = fr + 1, rec_stick_w[s] = w;
                wrote = 1;
            }
    }
    rec_last_w = w;
    for (int i = 0; i < 16; i++) {
        int bit = script_buttons[i].bit, down = (b >> bit & 1) != 0;
        if (down && !rec_down[i]) rec_down[i] = fr + 1, rec_down_w[i] = w;
        else if (!down && rec_down[i]) {
            rec_line(rec_down[i] - 1, rec_down_w[i], fr, w, script_buttons[i].name);
            rec_down[i] = 0;
            wrote = 1;
        }
    }
    const int32_t *st[2] = {sl, sr};
    for (int s = 0; s < 2; s++) {
        int moved = abs(st[s][0] - rec_stick[s][0]) > REC_STICK_STEP || abs(st[s][1] - rec_stick[s][1]) > REC_STICK_STEP ||
                    ((st[s][0] || st[s][1]) != (rec_stick[s][0] || rec_stick[s][1]));
        if (!moved) continue;
        if (rec_stick_from[s]) { rec_close_stick(s, fr, w); wrote = 1; }
        rec_stick[s][0] = st[s][0]; rec_stick[s][1] = st[s][1];
        if (st[s][0] || st[s][1]) rec_stick_from[s] = fr + 1, rec_stick_w[s] = w;
    }
    if (wrote) fflush(rec);
}
