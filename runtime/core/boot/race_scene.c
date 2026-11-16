// Whether the game is in a race (game.c: MKW_FPS's 20 and 15 apply only there; menus, movies and loads go at 30 at
// most, as on the console). Adrià's decision of 15/11/2026, after the cup's opening movie (KinokoOp) was not skipped
// when the interval was 4 as it opened (the opening itself now goes at 1: game.c, core/hacks/skip.c).
//
// The game's own sequence nodes say where it is (core/diag/seqwatch.c has the names and the vtable layout: the name
// getter's slot, then +10 slots the method that runs once when a node starts). seqwatch hooks every guest call, too
// dear to leave on; here only the start method's vtable slot of a few node classes points at a host function, which
// notes the event and calls the class's own method: no cost on any other call.
//
// The nodes and their roles: RACE_EVENTS in version.c. The map, the course intro and the opening arm it (out of the
// race), the first IsSequenceFlag after them is the countdown (in), and a finish, a GP's end or a menu ends it.
#include "host.h"
#include "version.h"

#define SLOT_ENTER 10

static volatile int in_race, armed, free_armed;
static uint64_t orig[RT_RACE_EVENTS];   // each patched class's own start method

int race_scene_on(void) { return __atomic_load_n(&in_race, __ATOMIC_RELAXED); }

// Event k's class started a node (x0): note it, then the class's own start method with the same arguments
static void node_start(Cpu *c, int k) {
    const RtRaceEvent *e = &rt_version()->race_events[k];
    int was = in_race;
    if (e->role == RT_RACE_ARM) armed = 1, in_race = 0;
    else if (e->role == RT_RACE_START && armed) armed = 0, in_race = 1;
    else if (e->role == RT_RACE_LEAVE) armed = free_armed = 0, in_race = 0;
    else if (e->role == RT_RACE_FREE_ARM) free_armed = 1, in_race = 0;
    // ChatUIReady starts at a race's finish (the standings over the course, vf1) and when free roam begins, right
    // after ReadyFreeRun (fl124a/b/f): free roam is driving, so it counts as a race
    else if (e->role == RT_RACE_CHAT) in_race = free_armed, free_armed = armed = 0;
    if (in_race != was) rt_log("race: %s at the start of %s", in_race ? "in a race" : "out of the race", e->name);
    rt_call(c, orig[k], 8, c->x[0], c->x[1], c->x[2], c->x[3], c->x[4], c->x[5], c->x[6], c->x[7]);
}

// One host address per event (patch page + RT_RACE_HOST + 0x10 * k): which class it is needs no vtable layout
#define H(k) static void host_##k(Cpu *c) { node_start(c, k); }
H(0) H(1) H(2) H(3) H(4) H(5) H(6) H(7) H(8) H(9) H(10) H(11) H(12) H(13) H(14) H(15)
#undef H
static void (*const hosts[RT_RACE_EVENTS])(Cpu *) = {host_0,  host_1,  host_2,  host_3, host_4,  host_5,
                                                     host_6,  host_7,  host_8,  host_9, host_10, host_11,
                                                     host_12, host_13, host_14, host_15};

void race_scene_host(uint64_t patch_page) {
    for (int k = 0; k < RT_RACE_EVENTS; k++) rt_register_host(patch_page + RT_RACE_HOST + 0x10u * (unsigned)k, hosts[k]);
}

// After seqwatch_init (it reads the slots this changes) and with main relocated
void race_scene_init(uint64_t patch_page) {
    const RtVersion *v = rt_version();
    int n = 0;
    for (; n < RT_RACE_EVENTS && v->race_events[n].name_slot; n++) {
        uint64_t *slot = (uint64_t *)(uintptr_t)(rt_main_addr(v->race_events[n].name_slot) + 8u * SLOT_ENTER);
        orig[n] = *slot;
        *slot = patch_page + RT_RACE_HOST + 0x10u * (unsigned)n;
    }
    if (!n) rt_log("race: no sequence nodes for game version %s: 20 and 15 fps apply everywhere", v->display);
}
