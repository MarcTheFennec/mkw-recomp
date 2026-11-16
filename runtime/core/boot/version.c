// The rows of version.h. 1.0.0's are the addresses the runtime had before there was a second version; 1.8.0's were
// found in its executable (11/11/2026) by the same features the 1.0.0 comments describe:
// - nvn_ret: main+0xdd07c8 is the first of the two nvnBootstrapLoader calls in the graphics bring-up (main+0xdd0720,
//   after nv::InitializeGraphics and nn::vi::GetDisplayVsyncEvent, as 1.0.0's main+0xcf39b4).
// - frame_update: main+0x69cbb0 is the only function of main using the frame object's +0x210 (fixed interval), +0x214,
//   +0x218, +0x128 and +0xf0 (game.c); vtable column main+0x367a9e0 slot 28, as 1.0.0's; the game clock is the u32
//   it adds [+0x214] to (adrp/ldr/add/str at main+0x69cd30).
// - zstd: recomp/patches-mkw-180.txt says how each function was found.
// - sdk: tools/gpu/nvn2_procs.py locate(), checked by disassembly.
// - shadow, seed, aspect and the menu focus probe (13/11/2026): recomp/patches-mkw-180.txt says how each site was
//   matched; rb_create is the only function using the string "shadow_map_depth", rb_alloc_init and rb_pool_init the
//   ones naming "agl::utl::DynamicTextureAllocator(DL)" and "...Allocator", rb_pool_new the operator new main+0xeb1a10
//   calls as 1.0.0's main+0x8eb85c does; the two sead::Random constructors are byte-identical to 1.0.0's;
//   ui_focus_fn has 1.0.0's shape (copies 16 bytes of x1 to this+0x3d0, tail call to a twin of main+0xce8620);
//   expheap_create is the one function the game's "RootHeap" creator (main+0xbeb470, as 1.0.0's main+0xcf3050) calls:
//   the same shape (a size check against the 0x120-byte header, the constructor, the free block). main carries two
//   heap libraries: the game's (pead, its heaps' layout: name at +0x20, lock flag at +0xc8) and a middleware's sead
//   (1.0.0 main+0x15511f4, 1.8.0 main+0x1699b30, layout 8 bytes longer: a SafeString name); the game's allocator
//   reads a heap by the first layout (s255h/s255i: a sead heap given to it crashed in main+0x247320).
#include <string.h>

#include "version.h"
#include "host.h"

// race_scene.c's nodes, by their name getter's vtable slot (build/seq_nodes.tsv, build/seq_nodes-180.tsv). A GP's
// order (dn1, dn2, 15/11/2026): ReadyRace (the world map), StartIntroMovie, WaitIntroMovie (the cup opening), then
// IsSequenceFlag ~130 frames after the movie (the countdown): the race; ChatUIReady at its finish, under the
// standings (vf1). Free roam: ReadyFreeRun, then ChatUIReady as it begins. StartRace and WaitRaceStart never start.
#define RACE_EVENTS(ready, start_movie, wait_movie, flag, finish, gp_end, gp_list, title, vs, tt, to_title, parts, \
                    chat, free)                                                                               \
    {{"ReadyRace", ready, RT_RACE_ARM}, {"StartIntroMovie", start_movie, RT_RACE_ARM},                       \
     {"WaitIntroMovie", wait_movie, RT_RACE_ARM}, {"IsSequenceFlag", flag, RT_RACE_START},                   \
     {"RaceFinishFaderOpenCheck", finish, RT_RACE_LEAVE}, {"GrandprixEndMovie", gp_end, RT_RACE_LEAVE},      \
     {"GrandprixListup", gp_list, RT_RACE_LEAVE}, {"TitleMenuMain", title, RT_RACE_LEAVE},                   \
     {"VSCourseSelect", vs, RT_RACE_LEAVE}, {"MenuTimeAttackMain", tt, RT_RACE_LEAVE},                       \
     {"ReturnToTitleMenu", to_title, RT_RACE_LEAVE}, {"PartsSelectEx", parts, RT_RACE_LEAVE},                \
     {"ChatUIReady", chat, RT_RACE_CHAT}, {"ReadyFreeRun", free, RT_RACE_FREE_ARM}}

static const RtVersion versions[] = {
    {
        .number = 100,
        .display = "1.0.0",
        .output_lines = 1440,
        .nvn_ret = 0xcf3a60,
        .frame_update = 0x3a9e28,
        .frame_slot = 0x329fd50,
        .game_clock = 0x33e6ae8,
        .zstd_multiframe = 0x407e00,
        .zstd_stream = 0x6532b4,
        .rb_create = 0x233358,
        .rb_alloc_init = 0x8eb6b0,
        .rb_pool_new = 0x67aa40,
        .rb_pool_init = 0x8eb9dc,
        .expheap_create = 0xcf30c0,
        .random_ctor_lr = 0x922088,
        .random_ctor2_lr = 0x15585a8,
        .ui_text_lr = 0x169aeb4,
        .aspect_first_lr = 0x460d44,
        .fov_entry_x = 24,   // main+0x45fba4: fmul s8, s0, s13, entry in x24
        .fov_half_v = 13,
        .ui_focus_fn = 0xce8564,
        .race_events = RACE_EVENTS(0x316e2a0, 0x3172538, 0x3177780, 0x316ac10, 0x316dfb0, 0x31697c0, 0x3169910,
                                   0x3174f08, 0x3176490, 0x316c268, 0x316f400, 0x316cf10, 0x3165fe0, 0x316e100),
        .sdk_name_pool = 0xb74c26,
        .sdk_name_offsets = 0xb79b90,
        .sdk_pointers = 0xd95168,
        .sdk_last_index = 0x2b9,
        .inits = {{"main", 2866}, {"subsdk0", 4}, {"sdk", 86}},
    },
    {
        .number = 180,
        .display = "1.8.0",
        .output_lines = 2160,
        .final_at_screen = 1,
        .nvn_ret = 0xdd07cc,
        .frame_update = 0x69cbb0,
        .frame_slot = 0x367aac0,
        .game_clock = 0x37c8438,
        .zstd_multiframe = 0xe2280,
        .zstd_stream = 0xdeca0,
        .rb_create = 0xb72c0,
        .rb_alloc_init = 0xeb1870,
        .rb_pool_new = 0x3e3550,
        .rb_pool_init = 0xeb1e60,
        .expheap_create = 0xbeb4e0,
        .random_ctor_lr = 0xbe9b78,
        .random_ctor2_lr = 0x16a0ef0,
        .ui_text_lr = 0x1805f20,
        .aspect_first_lr = 0x8a6d8,   // 1.0.0's 460d44 (its fdiv at 8a6d4)
        .fov_entry_x = 20,            // main+0x894a4, 1.0.0's 45fba4: fmul s8, s0, s14, entry in x20
        .fov_half_v = 14,
        .ui_focus_fn = 0x7fd810,
        .camera_service = 0x378ecb8,   // RELATIVE at main+0x36f4148; 9 functions load it
        .race_events = RACE_EVENTS(0x353f5c0, 0x35448c0, 0x354a5d8, 0x353b3a0, 0x353f180, 0x3539c68, 0x3539db8,
                                   0x3547698, 0x35492e8, 0x353ce78, 0x35409c8, 0x353df10, 0x3534d90, 0x353f420),
        .sdk_name_pool = 0xc26452,
        .sdk_name_offsets = 0xc2b870,
        .sdk_pointers = 0xe45aa8,
        .sdk_last_index = 0x2da,
        .inits = {{"main", 2960}, {"subsdk0", 0}, {"subsdk1", 4}, {"sdk", 88}},
    },
};

const RtVersion *rt_version(void) {
    static const RtVersion *cur;
    if (cur) return cur;
    int v = rt_game()->version;
    for (size_t i = 0; i < sizeof versions / sizeof *versions; i++)
        if (versions[i].number == v) return cur = &versions[i];
    rt_fatal("version: the game code is for game version %d, which this runtime has no addresses for", v);
}

uint64_t rt_main_addr(uint64_t off) {
    if (!off) return 0;
    for (int i = 0; i < ldr_count(); i++)
        if (!strcmp(ldr_module(i)->name, "main")) return ldr_module(i)->base + off;
    rt_fatal("version: main is not loaded");
}
