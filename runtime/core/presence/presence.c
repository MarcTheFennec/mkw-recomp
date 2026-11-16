// Discord Rich Presence: while the game runs, the player's Discord profile shows it (the name of the Discord
// application, the time played, an icon) and what the player is doing, from the files the game opens (below).
// MKW_DISCORD (Settings > General, off by default: the player's Discord friends see it) turns it on and off live; MKW_DISCORD_APP is the Discord application id, a test
// application's for instance.
//
// Discord's desktop client listens on a local pipe (Windows \\.\pipe\discord-ipc-N, Linux the Unix socket
// discord-ipc-N, N 0 to 9: one per running client). Each message is a frame: op (uint32 LE), length (uint32 LE) and
// that many bytes of JSON. The client sends HANDSHAKE (op 0, {"v":1,"client_id":...}), Discord answers a FRAME (op 1)
// with "evt":"READY", then each SET_ACTIVITY command (op 1) replaces the activity; CLOSE (op 2) ends the connection,
// PING (op 3) wants a PONG (op 4) with the same payload. Closing the pipe clears the activity. The protocol is
// Discord's (discord.com/developers/docs/topics/rpc); written here from that description, no SDK and no library: the
// same frames discord-rpc (MIT, github.com/discord/discord-rpc, archived) sends.
//
// Nothing of this runs on a game thread: the hooks only store the phase in atomics; one thread of its own connects,
// writes and reads. Without Discord, plat_ipc_open fails at once and the thread retries every RETRY_MS; a write that
// fails drops the connection the same way. The game never waits for Discord.
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "course_names.h"
#include "host.h"
#include "platform/platform.h"
#include "menu.h"
#include "presence.h"

#define TICK_MS 500          // how often the thread looks at the phase and reads what Discord sent
#define RETRY_MS 15000       // between attempts to reach Discord while it is not running
#define READY_MS 5000        // Discord answers the handshake within this or the connection is dropped
#define REFUSED_MS 300000    // after Discord closed the connection itself (an unknown application id): 5 minutes
#define MIN_UPDATE_MS 4000   // Discord takes 5 updates per 20 s; a race's load (race, then Time Trial) settles first

enum { OP_HANDSHAKE, OP_FRAME, OP_CLOSE, OP_PING, OP_PONG };

static const char DEFAULT_APP[] = "1571829149213925496";   // the project's Discord application, "Mario Kart World", with the
                                                            // port's icon (tools/build/icon.py) as the "icon" asset

static _Atomic unsigned changes;   // bumped by every hook that changed something
static _Atomic int connected;      // READY arrived on the current connection (presence_connected)
static PlatMutex course_lock = PLAT_MUTEX_INIT;
static char course[64];            // the course of the race as its file names say it ("MooMooFarm"); "" none

// ---- what the player does, from the files the game opens
//
// fs_host.c calls presence_file_opened for every file the game opens, on the game's threads: it only compares names
// and stores atomics (the course name behind a lock it never waits for). The files, from MKW_TRACE=fs runs of 1.0.0
// (15/11/2026: title, main menu, Time Trial on Moo Moo Farm, back to the menu); the game keeps most files it loaded once,
// so only those it opens again each time count:
//   Layout/MenuCupSelect_00...blarc    the menus' layouts, loaded at start and again on every return from a race
//   Env/Race_<Course>_<variant>[<world>]...   a race's lighting, at each race's load (the main menu's background
//                                      race opens Banc/Race_..., never Env/): the race starts, and its course
//   Env/InterStage..._<From>To<To>_... a Grand Prix's road to the next course
//   Movie/<Cup>Op.webm, SV0N_OP.webm   a cup's opening (Grand Prix) or a rally's (Knockout Tour), before its races
//   AI/RaceTimeAttackResult...ainb, Sound/.../BGM_Seq_RaceIntro..._TA.bwav   a Time Trial race's load
//   StaffGhost/<Course>.ghost.zs       a staff ghost raced or watched: Time Trial
//   Env/BalloonBattle_<Course>..., Env/CoinBattle_<Course>...   a battle (all of them also open once at start)
// The course is the game's own English name for the file's internal one ("MooMooFarm" -> "Moo Moo Meadows"), from the
// player's game texts (course_names.c), else the internal name's words ("Moo Moo Farm"): nothing of the game in this code.

enum { MODE_NONE, MODE_GRAND_PRIX, MODE_KNOCKOUT, MODE_TIME_TRIAL, MODE_BATTLE };

static _Atomic int mode;     // MODE_*: what the player chose, until the menus come back
static _Atomic int racing;   // 0 at start, 1 in the menus, 2 in a race (or a battle)

static int starts_with(const char *s, const char *head) { return !strncmp(s, head, strlen(head)); }

static int ends_with(const char *s, size_t n, const char *tail) {
    size_t t = strlen(tail);
    return n >= t && !memcmp(s + n - t, tail, t);
}

static void store(_Atomic int *a, int v) {
    if (atomic_exchange_explicit(a, v, memory_order_relaxed) != v)
        atomic_fetch_add_explicit(&changes, 1, memory_order_relaxed);
}

// The course from the part of a file name up to the next '_': "MooMooFarm_A_00" -> "MooMooFarm"; NULL clears it.
// The presence thread turns it into the game's name (course_names.c).
static void set_course(const char *stem) {
    char raw[sizeof course];
    size_t o = 0;
    for (size_t i = 0; stem && stem[i] && stem[i] != '_' && stem[i] != '[' && stem[i] != '.' && o + 1 < sizeof raw; i++)
        raw[o++] = stem[i];
    raw[o] = 0;
    if (!plat_mutex_trylock(&course_lock)) return;   // another opener holds it: the next file will do
    int changed = strcmp(course, raw) != 0;
    if (changed) memcpy(course, raw, o + 1);
    plat_mutex_unlock(&course_lock);
    if (changed) atomic_fetch_add_explicit(&changes, 1, memory_order_relaxed);
}

// The course a Grand Prix's road leads to: "InterStageEx1_ToadFactoryToChocoMountain_A_01" -> "ChocoMountain_A_01". The
// "To" between two courses follows a lowercase letter or a digit and comes before a capital, so the one that starts
// "ToadFactory" is not it (11 of 1.0.0's 202 roads start there); NULL when there is none.
static const char *interstage_to(const char *name) {
    const char *s = strchr(name, '_');
    for (s = s ? s + 1 : name; *s && *s != '_'; s++)
        if (s[0] == 'T' && s[1] == 'o' && s[2] >= 'A' && s[2] <= 'Z' && s > name &&
            ((s[-1] >= 'a' && s[-1] <= 'z') || (s[-1] >= '0' && s[-1] <= '9')))
            return s + 2;
    return NULL;
}

void presence_file_opened(const char *path) {
    const char *dir = strstr(path, ":/");   // "content:/Env/..." -> "Env/..."
    dir = dir ? dir + 2 : path;
    while (*dir == '/') dir++;
    const char *name = strrchr(dir, '/');
    name = name ? name + 1 : dir;
    size_t n = strlen(name);
    int started = atomic_load_explicit(&racing, memory_order_relaxed) != 0;
    if (starts_with(dir, "Layout/MenuCupSelect_")) {
        store(&mode, MODE_NONE);
        store(&racing, 1);
        set_course(NULL);
    } else if (starts_with(dir, "Env/Race_")) {
        set_course(name + 5);
        store(&racing, 2);
    } else if (starts_with(dir, "Env/InterStage")) {
        const char *to = interstage_to(name);
        if (to) set_course(to);
        store(&racing, 2);
    } else if (started && (starts_with(dir, "Env/BalloonBattle_") || starts_with(dir, "Env/CoinBattle_"))) {
        set_course(strchr(name, '_') + 1);
        store(&mode, MODE_BATTLE);
        store(&racing, 2);
    } else if (starts_with(dir, "Movie/") && ends_with(name, n, "_OP.webm")) {
        store(&mode, MODE_KNOCKOUT);
    } else if (starts_with(dir, "Movie/") && ends_with(name, n, "Op.webm")) {
        store(&mode, MODE_GRAND_PRIX);
    } else if (starts_with(dir, "AI/RaceTimeAttack") || (strstr(dir, "Sound/") && strstr(name, "RaceIntro") &&
                                                          ends_with(name, n, "_TA.bwav"))) {
        store(&mode, MODE_TIME_TRIAL);
    } else if (starts_with(dir, "StaffGhost/") && ends_with(name, n, ".ghost.zs")) {
        store(&mode, MODE_TIME_TRIAL);
    }
}

// The paths of a session as the game opens them, for --probe-discord.
const char *const presence_probe_steps[] = {
    "content:/Layout/MenuCupSelect_00.Nin_Ounce_NVN2.blarc.zs",
    "content:/Env/Race_MooMooFarm_A_00[Game03].dlpl.zs",
    "content:/AI/RaceTimeAttackResult.module.ainb",
    "content:/Layout/MenuCupSelect_00.Nin_Ounce_NVN2.blarc.zs",
    "content:/Movie/KinokoOp.webm",
    "content:/Env/Race_DKJungleParkway_A_00[Game03].dlpl.zs",
    "content:/Env/InterStageEx1_WestCityToCentral_A_01[Game03].dlpl.zs",
    "content:/Env/InterStageEx1_ToadFactoryToChocoMountain_A_01[Game03].dlpl.zs",
    NULL,
};

// ---- the connection

typedef struct {
    PlatIpc *ipc;
    char in[16384];   // received bytes not yet parsed into whole frames
    size_t n_in;
    int ready;        // READY arrived
    int closed;       // Discord sent CLOSE
} Conn;

static int send_frame(Conn *c, uint32_t op, const char *json, size_t n) {
    char *buf = malloc(8 + n);
    if (!buf) return 0;
    for (int i = 0; i < 4; i++) buf[i] = (char)(op >> 8 * i), buf[4 + i] = (char)((uint32_t)n >> 8 * i);
    memcpy(buf + 8, json, n);
    int ok = plat_ipc_send(c->ipc, buf, 8 + n);
    free(buf);
    return ok;
}

static uint32_t le32(const char *p) {
    const unsigned char *u = (const unsigned char *)p;
    return u[0] | u[1] << 8 | u[2] << 16 | (uint32_t)u[3] << 24;
}

static int has(const char *p, size_t n, const char *word) {   // memmem, which the Windows C runtime lacks
    size_t w = strlen(word);
    for (size_t i = 0; i + w <= n; i++)
        if (!memcmp(p + i, word, w)) return 1;
    return 0;
}

// Reads what has arrived and handles each whole frame; 0 when the connection is gone.
static int pump(Conn *c) {
    for (;;) {
        long got = plat_ipc_recv(c->ipc, c->in + c->n_in, sizeof c->in - c->n_in);
        if (got < 0) return 0;
        if (!got) break;
        c->n_in += (size_t)got;
        while (c->n_in >= 8) {
            uint32_t op = le32(c->in), len = le32(c->in + 4);
            if (len > sizeof c->in - 8) return 0;   // more than any answer to what we send: not Discord
            if (c->n_in < 8 + len) break;
            char *body = c->in + 8;
            if (op == OP_PING && !send_frame(c, OP_PONG, body, len)) return 0;
            if (op == OP_CLOSE) {
                rt_log("discord: closed by Discord: %.*s", (int)(len > 300 ? 300 : len), body);
                c->closed = 1;
                return 0;
            }
            if (op == OP_FRAME) {
                if (has(body, len, "\"READY\"")) c->ready = 1;
                else if (has(body, len, "\"ERROR\""))
                    rt_log("discord: error: %.*s", (int)(len > 300 ? 300 : len), body);
            }
            memmove(c->in, c->in + 8 + len, c->n_in - 8 - len);
            c->n_in -= 8 + len;
        }
        if (c->n_in == sizeof c->in) return 0;
    }
    return 1;
}

static const char *app_id(void) {
    const char *id = cfg_get("MKW_DISCORD_APP");
    return id && *id ? id : DEFAULT_APP;
}

static Conn refused_mark;
#define REFUSED (&refused_mark)

static Conn *connect_discord(void) {
    for (int i = 0; i < 10; i++) {
        char name[32];
        snprintf(name, sizeof name, "discord-ipc-%d", i);
        PlatIpc *ipc = plat_ipc_open(name);
        if (!ipc) continue;
        Conn *c = calloc(1, sizeof *c);
        if (!c) return plat_ipc_close(ipc), NULL;
        c->ipc = ipc;
        char hs[128];
        int n = snprintf(hs, sizeof hs, "{\"v\":1,\"client_id\":\"%s\"}", app_id());
        if (send_frame(c, OP_HANDSHAKE, hs, (size_t)n)) {
            for (uint64_t t0 = plat_now_ms(); plat_now_ms() - t0 < READY_MS && pump(c) && !c->ready;) plat_sleep_ms(50);
            if (c->ready) {
                rt_log("discord: connected (%s, application %s)", name, app_id());
                return c;
            }
        }
        rt_log("discord: %s did not answer the handshake%s", name, c->closed ? " (closed)" : "");
        int refused = c->closed;
        plat_ipc_close(c->ipc);
        free(c);
        if (refused) return REFUSED;   // Discord refused this application: no other client will accept it
    }
    return NULL;
}

// ---- the activity

// s as a JSON string's contents into out (quotes, backslashes and control characters escaped).
static void json_str(char *out, size_t cap, const char *s) {
    size_t o = 0;
    for (; *s && o + 7 < cap; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') out[o++] = '\\', out[o++] = (char)ch;
        else if (ch < 0x20) o += (size_t)snprintf(out + o, cap - o, "\\u%04x", ch);
        else out[o++] = (char)ch;
    }
    out[o] = 0;
}

// A course the game's texts do not name, as its internal name's words: "DKJungleParkway" -> "DK Jungle Parkway".
static void words(const char *raw, char *out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; raw[i] && o + 2 < cap; i++) {
        char ch = raw[i];
        int upper = ch >= 'A' && ch <= 'Z', next_lower = raw[i + 1] >= 'a' && raw[i + 1] <= 'z';
        int prev_lower = i && raw[i - 1] >= 'a' && raw[i - 1] <= 'z';
        if (o && upper && (prev_lower || next_lower)) out[o++] = ' ';
        out[o++] = ch;
    }
    out[o] = 0;
}

// What the profile says: details (the phase) and state (the course, or the port's settings). In English whatever the
// menu language, as the course names are (the user, 15/11/2026).
static void describe(char *details, size_t dcap, char *state, size_t scap) {
    int m = atomic_load_explicit(&mode, memory_order_relaxed), r = atomic_load_explicit(&racing, memory_order_relaxed);
    static const char *const modes[] = {"Racing", "Grand Prix", "Knockout Tour", "Time Trial", "Battle"};
    snprintf(details, dcap, "%s", !r ? "Starting the game" : r == 1 ? "In the menus" : modes[m]);
    *state = 0;
    if (menu_is_open()) snprintf(state, scap, "In the port's settings");
    else if (r == 2) {
        char raw[sizeof course];
        plat_mutex_lock(&course_lock);
        memcpy(raw, course, sizeof raw);
        plat_mutex_unlock(&course_lock);
        if (*raw && !course_name(raw, m == MODE_BATTLE, state, scap)) words(raw, state, scap);
    }
}

const char *presence_describe(void) {
    static char out[300];
    char details[128], state[128];
    describe(details, sizeof details, state, sizeof state);
    snprintf(out, sizeof out, "%s%s%s", details, *state ? " / " : "", state);
    return out;
}

int presence_connected(void) { return atomic_load_explicit(&connected, memory_order_relaxed); }

// SET_ACTIVITY with what the player does now; on == 0 clears the activity.
static int send_activity(Conn *c, int on, int64_t start_s) {
    static unsigned nonce;
    char json[2048], activity[1200] = "null";
    if (on) {
        char d[128], st[128], details[256], state[256];
        describe(d, sizeof d, st, sizeof st);
        json_str(details, sizeof details, d);
        json_str(state, sizeof state, st);
        snprintf(activity, sizeof activity,
                 "{\"details\":\"%s\"%s%s%s,\"timestamps\":{\"start\":%lld},"
                 "\"assets\":{\"large_image\":\"icon\",\"large_text\":\"mkw-recomp %s\"}}",
                 details, *state ? ",\"state\":\"" : "", state, *state ? "\"" : "", (long long)start_s, rt_port_version);
        rt_log("discord: %s%s%s", d, *st ? " / " : "", st);
    }
    int n = snprintf(json, sizeof json, "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%u,\"activity\":%s},\"nonce\":\"%u\"}",
                     plat_process_id(), activity, ++nonce);
    return n > 0 && (size_t)n < sizeof json && send_frame(c, OP_FRAME, json, (size_t)n);
}

static void presence_main(void *arg) {
    (void)arg;
    int64_t start_s = (int64_t)(plat_unix_time_ns() / 1000000000u);   // the time played counts from the game's start
    Conn *c = NULL;
    uint64_t next_try = 0, last_sent = 0;
    unsigned sent_changes = 0;
    int shown = 0;      // Discord has our activity
    int menu_was = 0;   // menu_is_open() at the last tick: the F1 menu (ui/menu)
    for (;; plat_sleep_ms(TICK_MS)) {
        int on = cfg_bool("MKW_DISCORD");
        uint64_t now = plat_now_ms();
        if (!c) {
            if (!on || now < next_try) continue;
            c = connect_discord();
            if (c == REFUSED) c = NULL, next_try = now + REFUSED_MS;
            else if (!c) next_try = now + RETRY_MS;
            if (!c) continue;
            shown = 0;
            atomic_store_explicit(&connected, 1, memory_order_relaxed);
        }
        int ok = pump(c);
        int menu = menu_is_open();
        if (menu != menu_was) menu_was = menu, atomic_fetch_add_explicit(&changes, 1, memory_order_relaxed);
        unsigned ch = atomic_load_explicit(&changes, memory_order_relaxed);
        if (ok && !on) {
            send_activity(c, 0, start_s);   // the setting turned off: clear the activity, then let go of Discord
            rt_log("discord: off (MKW_DISCORD=0)");
            ok = 0;
        } else if (ok && (!shown || ch != sent_changes) && now - last_sent >= MIN_UPDATE_MS) {
            sent_changes = ch;
            ok = send_activity(c, 1, start_s);
            shown = 1, last_sent = now;
        }
        if (!ok) {
            next_try = c->closed ? now + REFUSED_MS : on ? now + RETRY_MS : 0;
            if (on) rt_log("discord: connection lost; trying again in %d s", (int)((next_try - now) / 1000));
            atomic_store_explicit(&connected, 0, memory_order_relaxed);
            plat_ipc_close(c->ipc);
            free(c);
            c = NULL;
        }
    }
}

void presence_start(void) {
    static _Atomic int started;
    if (atomic_exchange(&started, 1)) return;
    if (!plat_thread_spawn(presence_main, NULL, "discord", 0)) rt_log("discord: no thread");
}
