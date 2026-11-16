# runtime/core/hacks

Shortcuts the console does not have, for playing and above all for developing faster. All off by default, each with
its `MKW_*` and its row in the "Hacks" tab (`ui/menu/settings_tabs.cpp`).

| File | What it is |
|---|---|
| `hacks.h` | The hacks' interface |
| `turbo.c` | Turbo (`MKW_TURBO` off/hold/toggle/on/until, `MKW_TURBO_SPEED`, F2 key): the pacer, the vsync thread and the guest clock (`rt_ticks`) run N times faster; the window shows at most 60 frames per second |
| `skip.c` | Skip movies (`MKW_SKIP_MOVIES`): presses A while a cup intro plays (`*Op.webm`), until the game closes its decoder; and, skipped or not, tells `core/boot/game.c` an intro is on (it runs at 60 then, so it can be skipped) |
| `seed.c` | Same randomness (`MKW_SEED=<number>`): the system entropy (svc `GetInfo 11`) and the 21 clock seedings of sead's generator (patch set `seed`) come from that seed: the title screen always shows the same course |
| `unlock.c`, `unlock.h`, `unlock_ids.c` | Unlock everything (`MKW_UNLOCK_ALL`): edits `Play/account.sav` when it is opened, like Ca1nM's save editor (MIT) |
| `freeze.c` | Freeze (`MKW_FREEZE_KEYS`, F3/F4; `ctl.sh NAME pause`, `step [N]`, `resume`): the game thread waits in its present |
| `classic.c` | Classic Grand Prix (`MKW_CLASSIC_GP`): every race of the 8 Grand Prix cups is laps on one course; Classic VS (`MKW_CLASSIC_VS`): so is every VS race, each area's routes (CourseEx) becoming their destinations' courses; each intermission's CourseId becomes its destination's course, in the `general_format` archive as the host zstd decoder writes it (on at start) and, switched in the menu, in the game's own copies of the cups after a background scan; races started before it finishes keep their course |
