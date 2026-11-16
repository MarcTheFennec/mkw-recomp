# runtime/core/boot

Boot: the program entry point, the game code tables, module loading, patches and the game clock.

| File | What it is |
|---|---|
| `main.c`, `boot.c` | Program entry (`MKW_ALLOW_PTRACE=1` lets gdb attach; before it, the first constructor tells a CPU without AVX2 so in a box) and boot sequence; `--boot` stops at MKW's first NVN call (1.0.0 `main+0xcf3a5c`, 1.8.0 `main+0xdd07c8`) |
| `gamelib.c`, `gamelib.h` | Game code tables (modules, functions, ExeFS hashes, patches): linked into `build/mkw`, or loaded from `mkwgame.so`/`mkwgame.dll` in the selected installation folder in the player's build (`build.py --dist`), with its header id |
| `loader.c` | Loads the modules at their addresses, relocations, import binding, constructor flag (with Switch 2's `PACIBSP`); `--probe-loader` compares against MKW 1.0.0's numbers (another version's are printed only) |
| `game.c` | MKW's own: the game clock (`frame_step_*`, counts presents; totk-pc-recomp takes it from `frame_rate.c` and its patches); the world clock of A/B runs (`MKW_AB`) counts from the `MKW_SEQ_UNTIL` event; the frame rate setting (`MKW_FPS` auto/game/60/30/20/15: the game's present interval, through its frame object's vtable slot) and the `game: speed` line every 5 s |
| `race_scene.c` | Whether the game is in a race, from the start of a few of its sequence nodes (their start method's vtable slot points at a host function, no cost elsewhere): `MKW_FPS`'s 20 and 15 apply only there, 30 at most in menus, movies and loads (`game.c`) |
| `fps_auto.c` | `MKW_FPS=auto`: the present interval 1-4 (60 to 15 fps) from the time the game measured for each frame and the real frame period, with hysteresis and a hold after each step up (`--probe-fps`). `game.c` writes only its 3 and 4 (in a race, without frame generation): at 1 or 2 the game picks each frame |
| `frame_jit.c` | `MKW_LOW_LATENCY`: the game's acquire waits until the next display slot minus the frame's measured work (acquire to GPU done, 90th percentile, plus a margin), so the frame and its controller read are younger on screen |
| `version.c`, `version.h` | The fixed guest addresses of each game version (1.0.0, 1.8.0), chosen by `RtGame.version`: the frame update, the game clock, zstd, the NVN2 proc table, the boot checkpoint and `init_array` sizes; a field at 0 turns its feature off |
| `patches.c` | Patches by address; opt-in tall Window (1.0.0 and 1.8.0) requires the regenerated `vertplus` set (old tables keep 16:9), preserves horizontal FOV and fits controls, while full-canvas backdrops, character/kart capture masks and the cup side backdrop cover the output |
