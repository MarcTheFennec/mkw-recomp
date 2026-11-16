# recomp

Generator: translates the AArch64 code of the game's NSOs (`main`, `subsdk0`, `sdk`; 1.8.0 adds `subsdk1`) to C, one C
function per game function.

Copied from [totk-pc-recomp](https://github.com/valentderah/totk-pc-recomp) `recomp/` @ c3b9a4e (MIT, license in
`LICENSE.totk-pc-recomp`; they took it in turn from LAS Recomp). Without their TotK patches (`patches-v*.txt`); MKW's
is `patches-mkw.txt`. Kept close to the original so the two can be compared; ruff does not check it (`pyproject.toml`).

Changes against the original:

- `lifter.py`: `RETAA`/`RETAB` as `ret`, `LDAPR*` as a load-acquire, `PAC*`/`AUT*`/`XPAC*` as identity (Switch 2's
  ARMv8.3).
- `patches-mkw.txt`: the `vertplus` set supplies the late race-FOV hook for tall Window mode (also in
  `patches-mkw-180.txt`). Without this regenerated set the runtime keeps tall windows at 16:9. Besides the spin waits and zstd, the
  `output` set (the game's output size, `patches.c`) and the
  `shadow` set (the shadow map's size, and a second pool slot for the render-buffer allocator to hold it), and the `lan`
  set (`MKW_LAN=1`: the title menu opens with LAN Play chosen, as after its L+R+left stick combo).
- `gen.py`: the `GENERATED_MARKER` (`MKW_RECOMP_…`) lives in the file itself instead of `tools/check_content.py`;
  `tools/git/pre-commit` watches for it. Every back edge of a loop without calls carries a compiler barrier
  (`__asm__ volatile("" ::: "memory")`): without it clang hoists out of the loop the loads another thread changes and
  leaves a `jmp` to itself (`main+0x27ebb20`, which `spin_waits.py` does not recognise).

Usage: `uv run python recomp/gen.py --exefs game/exefs --out build/generated --jobs 8 --patches recomp/patches-mkw.txt`
(~2.5 min, 887 MB of C); for 1.8.0, `--patches recomp/patches-mkw-180.txt` (`uv run recomp/versions.py game/exefs`
names the dump's version and its table).

| File | What it does |
|---|---|
| `nso.py` | Loads an NSO: LZ4 segments, image, MOD0, symbols and relocations |
| `module.py` | A module loaded at a base: `.text` words, function seeds (`.eh_frame_hdr`, symbols, relocations) |
| `lifter.py` | AArch64 decoder → C statements per instruction |
| `simd_c.py` | NEON/FP → C |
| `flags.py`, `regs.py` | NZCV and the most used registers in local variables |
| `jumptables.py` | Jump tables resolved symbolically |
| `gen.py` | Generates the C by function, in parallel processes |
| `layout.py` | Load addresses of the modules |
| `patches.py` | Instruction patches by address and spin loops (`spin`) |
| `versions.py` | The game versions the port knows, by `exefs/main`'s build id: name, `RtGame.version` number, RomFS size, patch table |
| `patches-mkw.txt` | MKW 1.0.0's table: the 117 spin loops `tools/cpu/spin_waits.py` finds (30/10/2026). Without it clang hoists the load out of the loop and leaves a `jmp` to itself (thread 89 at `main+0xe32c0c`, a spinlock). And the `zstd` set (`MKW_ZSTD_NATIVE`): the two calls to `ZSTD_decompressMultiFrame` and the streaming path's (`main+0x6532b4`) go to `runtime/sdk/ov/ov_zstd.c` |
| `patches-mkw-180.txt` | MKW 1.8.0's table: its 97 spin loops (`spin_waits.py`, 11/11/2026, over `main`, `subsdk0`, `subsdk1`, `sdk`), the `zstd` set and the `output` set found again in that executable (the file says how), and the `camera` set (`MKW_CAMERA_SKIP`, on): the USB camera service not opened, its event and service object stand-ins (`runtime/core/boot/patches.c`). Every set of 1.0.0 too (`aspect`, `shadow` and `seed` since 13/11/2026, `lan` since 15/11/2026): with `MKW_LAN=1` its title menu opens with LAN Play chosen, as 1.0.0's does (#309) |

Coverage over MKW 1.0.0: `tools/cpu/lift_coverage.py`.
