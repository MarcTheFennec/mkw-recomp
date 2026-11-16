# Initial research (30/10/2026)

Game copy: `exefs/` 40 MB + `romfs/` 23 GB, 205 781 files. Program id `0400C3F00006E000`.

## The executable

Measured with the `recomp/nso.py` loader (from totk-pc-recomp) on the decompressed NSOs.

| Module | Text | Image | Dynamic symbols | What it is |
|---|---|---|---|---|
| `main` | 42 MB | 54 MB | 1 289 imported, 151 exported | the game |
| `sdk` | 7.8 MB | 14 MB | 27 427 exported | SDK 19.3.3, `spec: Ounce` (Switch 2), NVN2 driver inside |
| `subsdk0` | 2.6 MB | 3.6 MB | 216 imported | Mono (.NET) runtime; `main` imports nothing from it except `nn::ecli::Allocate/Deallocate` |
| `rtld` | 8 KB | 64 KB | — | dynamic linker; not needed in a recomp |

- The same NSO0 (LZ4) and MOD0 format as Switch 1. `main.npdm`: Switch 2 ACID (signature area 0x300, not 0x200),
  cores 0-5 (mask 0x3f), new services `banana`, `audsmx:u`, `ngc:u`, `notif:a`, `ectx:aw`.
- `main`'s imports by namespace: `nn::audio` 234, `nn::os` 127, `nn::codec` 77, `nn::hid` 57, `nn::movie` 52,
  `nn::util` 48, `nn::async` 44, `nn::socket` 32, `nn::fs` 27… NVN2 comes in through `nvnBootstrapLoader` +
  `nv::InitializeGraphics` (as NVN1).
- NVN2: ~701 function names in `sdk` (`CommandBuffer*` 202, `Texture*` 97, `Device*` 61, `Dlss*` 54, ray tracing
  acceleration `CommandBufferBindAccelerationStructure*`). Approximate regex.
- Engine: Nintendo EPD's. In `romfs/`: `RSDB`, `Phive`, `Banc`, `AI` (`.ainb`), `GymlArchive`, `Lib/{agl,gsys,sead}`,
  shaders `*.Nin_Ounce_NVN2.bfsha.zs`, 68 % of the files as `.zs` (zstd). In `main`: `sead::` 16 strings, `agl::` 40,
  `gsys::` 23, `phive` 311, Havok `hknp` 1 100, `ainb` 85, `gyml` 119. No RTTI.

## Switch 2 hardware (T239)

- CPU: 8 × Cortex-A78C (6 for the game), ARMv8.2 + PAC (v8.3/v8.6). ~1 GHz.
- GPU: Ampere, 1 536 CUDA cores (12 SMs, own count), RT and tensor (DLSS). Shaders in SM86 SASS.
- 12 GB LPDDR5X, 102 GB/s.
- Sources: [Switchbrew T239](https://switchbrew.org/wiki/Switch_2:_Tegra_T239),
  [TechSpot](https://www.techspot.com/news/107922-nintendo-finally-clarifies-switch-2-custom-nvidia-processor.html).

Instructions in `main` that a Switch 1 lifter lacks (count from eden-nx2, `NX2_NOTES.md` row "Titles"): RETAA/B
114 767, LDAPR 37 560, LSE atomics 24 718, CAS 6 640, SWP 2 470, CASP 6, XPAC 7, PAC 2, STG 1. PAC is treated as
identity and MTE tags as 0 (eden-nx2, row 3).

## Sources

Every source used, with its license and what is taken from it. From MIT sources code may be taken, citing it; from
GPL ones and from those without a license, only ideas.

| Source | License | What is used |
|---|---|---|
| [Atmosphere-NX/Atmosphere](https://github.com/Atmosphere-NX/Atmosphere) `libmesosphere` (master, 12/11/2026) | GPL-2.0 | Only knowledge of the Switch kernel's argument checks: `SetThreadCoreMask` (`kern_svc_thread.cpp`: InvalidCoreId for a mask or core outside the process, InvalidCombination for an empty mask or the core outside it; -1 = the process's ideal core alone) and `CreateThread`'s core range |
| [Takoroka/eden-nx2](https://git.eden-emu.dev/Takoroka/eden-nx2), branch `eden-nx2` (50fa86bb, 29/10/2026) | GPL-3.0 | **The first to run MKW** (an emulator): title, Mushroom Cup movie and race, ~17-25 ms/frame on an RTX 4070 laptop. `NX2_NOTES.md`: 149 blockers with cause and fix: protocol of the new services (`gsv`, `vi2`, `syncpt`, `nvsrv`, `gpu` 101/211/212/214/215), memory (12 GB, sparse regions of ~20 GB), Ampere GPU (classes C797/C7C0/C7B5, Turing MME64 macros, QMD 3.0), lowering Ampere SASS to Maxwell (`shader_recompiler/frontend/ampere/lower_to_maxwell.h`, 2 249 lines), ASTC with Switch 2's swizzle, VP9 video. Taken as ideas only: Maxwell's `InstanceId` is `InstanceIndex - BaseInstance` in Vulkan → `sm86/translate/glsl/mem.py`; an `LDG`/`STG` whose base is not tracked searches its buffer among the program's at run time → `global_search` in `sm86/translate/glsl/mem.py`; "read traps" (what the GPU writes reaches game memory when the game touches it) → `mem_trap` in `runtime/core/memory/memory.c`, our own code; the alpha test as a discard in the FS before writing, with function and reference fixed in the pipeline → `spirv_alpha_test` in `runtime/gfx/vk/pipe/vk_spirv_patch.c`, our own code; Maxwell's `KIL` as a demote to helper invocation, not `OpKill` (`emit_spirv_control_flow.cpp`) → `KILL` in `sm86/translate/glsl/core.py` (#22) |
| [Discord RPC](https://discord.com/developers/docs/topics/rpc) and [discord/discord-rpc](https://github.com/discord/discord-rpc) (archived, 3.4.0) | Discord's documentation; MIT | The protocol only, our own code (`runtime/core/presence/presence.c`): the local pipe `discord-ipc-N` (named pipe on Windows, Unix socket under `$XDG_RUNTIME_DIR` and the Flatpak and Snap folders on Linux), frames of op and length, the handshake, `SET_ACTIVITY` with the process id, PING/PONG and CLOSE |
| [Super Mario Wiki](https://www.mariowiki.com/), the "Internal names" tables of [Big Donut](https://www.mariowiki.com/Big_Donut), [Chain Chomp Desert](https://www.mariowiki.com/Chain_Chomp_Desert), [Dino Dino Jungle](https://www.mariowiki.com/Dino_Dino_Jungle) and [DK Pass](https://www.mariowiki.com/DK_Pass) (16/11/2026) | CC BY-SA 3.0 | Facts only: which battle course each internal name is (Volcano, Desert, Jungle, Ice, from the thumbnails' file names `C_BattleL_<name>_00`) → `battles[]` in `runtime/core/presence/course_names.c`; the names themselves come from the player's game texts |
| [nothings/stb](https://github.com/nothings/stb) `stb_image.h` v2.30 @ 048d2aa5 (28/10/2026) | MIT or public domain | Code, unchanged in `runtime/third_party/stb/`: the JPEG and PNG decoder of the Mods page's GameBanana pictures |
| [SavageCore/switch2-mod-merger](https://github.com/SavageCore/switch2-mod-merger) (11/2026) | MIT | Code, ported to Python by its author in PR #218: `tools/mod/merge.py` merges mods that replace the same `.sarc` member by member, takes a member changed by one mod, and asks for a winner (or the game's own) when several change it; `build_sarc` lays the archive out as its `buildSarc` |
| [NotRexed/Hookshot](https://github.com/NotRexed/Hookshot) 779bdcf (10/11/2026) | GPL-3.0 | Ideas only. Its `setjmp`/`longjmp` in lifted code (`host/hos_setjmp.c`): the guest's longjmp restores the registers itself, and its final `BR x30` into the middle of a function is caught and turned into a host longjmp back to a frame that is still alive → `runtime/cpu/sjlj.c`, our own code (the frame is the setjmp caller's own; every MKW owner works before it returns). Its update offer (`host/hos_update.c`): at start, GitHub's latest release (a few seconds at most, nothing offline); when it differs from the one the folder was built with, the player is asked, and yes rebuilds the folder from the game files it keeps → the launcher's update check, our own code |
| [tangerineturtle1/NX2navi](https://github.com/tangerineturtle1/NX2navi) f58ad1d (09/11/2026) | MIT for the imported base only (0478c13); later additions without a license (`LICENSE-ADDITIONS`, "All rights reserved"): ideas only | A Switch 2 Ocarina of Time recomp on totk-pc-recomp (NVN2 over Vulkan). Reviewed in full (09/11/2026): six bugs of our translator, five Linux kernel and fiber bugs inherited from totk, a Linux package with an old sysroot (MIT base `tools/make_release_linux.py`), differential tests of the lifter and `pick_jobs` (MIT base), a robust setup. The MIT equivalents are taken from up-to-date totk-pc-recomp |
| [craftycorvid/oot-mod](https://github.com/craftycorvid/oot-mod) c5ccd1c (11/11/2026, through NX2navi's notes) | MIT | Its 2D canvas hook for Ocarina of Time's ultrawide: a full-screen orthographic layout projection gets its x row scaled by 16:9 over the picture's aspect. The idea only, rewritten for MKW's nn::ui2d draw setup (`runtime/core/boot/patches.c` host_ui_proj, MKW_ASPECT). Tall Window mode uses the same canvas-fit idea on the y row; its race FOV correction and backdrop/mask classification are original code grounded in MKW 1.0.0 |
| [STS78721/MKW-THWOMP](https://github.com/STS78721/MKW-THWOMP) (09/11/2026) | GPL-3.0 as a whole (Eglantine's translator); what it inherits from totk-pc-recomp, MIT | Another MKW recomp (1.8.0) for Windows: a fork of DKB Recomp, ported in a day; intro only. Reviewed (09/11/2026): push constant bank 24, compute c0 table (fixed here), audio: ideas only. Its `HANDOVER.md` (13/11/2026): the same NVIDIA "invalid instruction pointer" loss on quad draws whose vertex stage reads c9 at a computed index, and its SPIR-V alone does not fault (issue #22): a finding, no code |
| [valentderah/totk-pc-recomp](https://github.com/valentderah/totk-pc-recomp) c3b9a4e | MIT | Same engine. AArch64→C lifter in Python (`recomp/`, from LAS: registers in locals, symbolic jump tables, LDXP/STXP), SDK replaced by `nn::*` function name, virtual cores with preemption (`runtime/kernel.c`), the game's zstd to libzstd (`ov_zstd.c`), NVN1→Vulkan (`vk_*.c`), spin loops (`tools/spin_waits.py`). See its `docs/architecture.md`. Taken (design, our code): the GPU breadcrumbs of `runtime/vk_crumbs.c` (710b7f5) in `runtime/gfx/vk/diag/vk_crumbs.c`. Taken (code, cited; 7a37fc3, 12/11/2026): the in-game menu on an HDR10 swapchain (`runtime/vk_hdr.c` `vk_hdr_ui_*`, `shaders/hdr_ui.frag`, `hdr_fullscreen.vert`) in `runtime/gfx/vk/device/vk_hdr_ui.c` and `runtime/shaders/`, and its PQ to sRGB conversion (`vk_hdr_to_srgb8`) in `gfx_hdr.c`; its picture encoding is not taken (MKW draws its own PQ picture). Taken (code, cited; 12/11/2026): the save commit fixes e45eb07, 65fd631, d726630, d3f403e (`runtime/fs_host.c`, `plat_fs_busy_reason`, `plat_fs_os_error`, read-only folders and cloud placeholders) in `runtime/sdk/fs/fs_host.c`, `win32_sys.c`, `linux_fs.c`; the delay ring fix d22a19c in `runtime/audio/audio_effects.c` and `audio_reverb.c` |
| [totk-pc-recomp#60](https://github.com/valentderah/totk-pc-recomp/pull/60), branch `wdiasjunior/linux-port` @ eccf4dd | MIT | Linux port of totk-pc-recomp's runtime: `platform/linux.c` (futex, userfaultfd, signals, fibers), SDL3, AppImage. Tested on Arch + RADV at 60 fps. The base of this repo's runtime. `tools/crit_path.py` (critical path of `MKW_CRIT`) → `tools/run/crit_path.py` |
| totk-pc-recomp `tools/make_release.py` and `tools/build_game.py` (c3b9a4e / eccf4dd) | MIT | Windows build: llvm-mingw plus the MSYS2 UCRT64 packages with their sha256 (`PINS`) and the link libraries (`LLVM_LINK_LIBS`) → `tools/build/windeps.py` and `build.py --target windows`, cited |
| totk-pc-recomp `tools/make_release_linux.py` (ae30a61) | MIT | The Linux release: built against the Steam Runtime 3 "sniper" SDK sysroot (the same pinned snapshot), zstd built static, and packed as an AppImage with appimagetool 1.9.1 and the static type-2 runtime 20251108, each pinned by sha256 → `tools/build/sysroot.py` and `tools/build/release.py`, cited |
| [Steam Runtime 3 "sniper" SDK](https://gitlab.steamos.cloud/steamrt/sniper/sdk) sysroot 3.0.20261021.266025, [SDL](https://github.com/libsdl-org/SDL) 3.4.18, [AppImage](https://github.com/AppImage/appimagetool) tools | Debian packages' own licenses (Opus BSD-3, libvpx BSD-3, glibc LGPL, only linked against) / Zlib / MIT | The Linux release links against the sysroot (glibc 2.31) and bundles its Opus and libvpx, and SDL3 built from source against it; their licences go into the AppImage's `licenses/` |
| totk-pc-recomp `installer/` (c3b9a4e) | MIT | An installer over a CLI that emits one event per line, steps with a key to resume, a portable folder with rotated `logs/`: the model of `tools/prepare/prepare.py` and of how `runtime/ui/launcher/launcher_setup.cpp` reads it |
| [mstorsjo/llvm-mingw](https://github.com/mstorsjo/llvm-mingw) 20261020 (Linux build `ucrt-ubuntu-22.04-x86_64.tar.xz` and Windows build `ucrt-x86_64.zip`) | Apache-2.0 with LLVM exception; permissive mingw-w64 parts | The toolchain that builds `build/mkw.exe` (clang 23, lld, UCRT, libc++): the Linux build cross-compiles it, the Windows build compiles it on Windows itself; both pinned by sha256 in `tools/build/windeps.py`; no code copied |
| MSYS2 UCRT64 (`opus`, `libvpx`, `zstd`, `xxhash`, `vulkan-headers`, `vulkan-loader`, `vulkan-memory-allocator`) and SDL 3.4.18 (`SDL3-devel-…-mingw`) | BSD-3 / BSD / BSD / BSD-2 / Apache-2.0 / Apache-2.0 / MIT / zlib | Windows headers, import libraries and DLLs of the same libraries linked on Linux (`build/win/deps`) |
| [Ryujinx](https://git.ryujinx.app/ryubing/ryujinx) (copy in totk-pc-recomp `tools/vendor/ryujinx`, 1.3.2) | MIT | Semantics of NVIDIA shaders for the SM86 translator: SPH header (`Translation/ShaderHeader.cs`), `IPA` with perspective and `gl_FragCoord.w` (`Instructions/InstEmitAttribute.cs`), `LOP3`'s predicate (`InstEmitIntegerLogical.cs`), `TMML` (`InstEmitTexture.cs`), global memory as storage buffers (`GlobalMemoryToStorageBufferPass`). It is Maxwell: what is Ampere's is checked against Mesa NAK |
| Ryujinx `Ryujinx.HLE/HOS/Services/Sockets/Bsd/Types` (4d5a19d) | MIT | Values only, no code: the console's FreeBSD socket constants (option levels and names, message flags, the type's creation flags, eventfd flags, `sockaddr_in` layout, poll events) and its Linux errno numbering → `runtime/sdk/ov/ov_socket.c` |
| Ryujinx `Ryujinx.Graphics.Gpu/Image/Texture.cs` (475615f) | MIT | Only the idea: without native ASTC, decode and recompress to BC7 (`EnableTextureRecompression`). The mode 6 BC7 encoder of `runtime/shaders/bc7_encode.comp` is our own |
| [richgel999/bc7enc_rdo](https://github.com/richgel999/bc7enc_rdo) (b9438627) | MIT (`bc7e.ispc`: Apache-2.0, not used) | Code: `bc7enc.cpp` as is in `runtime/third_party/bc7enc`, the offline BC7 encoder of `vk_texpre.c` (modes 1, 5, 6, 7) |
| [Mesa NAK](https://gitlab.freedesktop.org/mesa/mesa/-/tree/main/src/nouveau/compiler) (`nak_nir_lower_tex.c`) | MIT | The free NVIDIA compiler for SM70+ (Volta-Ada, Ampere included): `TEX` operand order (group 0 = array index and coordinates, group 1 = LOD/bias, offset, comparison) and the scalar `.SCR` form; encoder and semantics of each instruction |
| smo-recomp (Coz-0, ssbu-recomp lineage) | GPL-3.0 | A sibling recomp by the same author: lifter with a differential test against Unicorn, NVN→Vulkan runtime with its own Maxwell translator, measured improvements. Ideas only |
| SavageWindRecomp (BotW, LAS runtime) @ 663cff16 | MIT | A sibling recomp by the same author. Taken: the structured control flow of its shader translator (`structure_flow`, in `sm86/translate/flow.py`); `FMNMX` and `.SAT` as GLSL.std.450 `NMin`/`NMax`/`NClamp` (`runtime/nvn/maxwell/translate.c`, in `sm86/translate/glsl/common.py` `NMINMAX_FN`); its Windows build docs as a comparison |
| [Revivedx/SavageWindRecomp](https://github.com/Revivedx/SavageWindRecomp) (`main` @ 5173d27) | MIT | Start screen and building the game on the player's machine: `runtime/ui/launcher.mm` (pages, game files, version check by the NSO build id and the RomFS file count), `runtime/gamelib.*` (game code as a library with a single entry by name and a header id) and the zig options of `runtime/gamebuild.c`. The idea and those options are taken (`runtime/ui/launcher/launcher.cpp`, `runtime/core/boot/gamelib.c`, `tools/prepare/prepare.py`) |
| SavageWindRecomp `runtime/nvn/glyphs.c` (branch `exp/recorder`) | MIT | Button icons matching the controller by redrawing the game's icon sheet on upload: the idea and the signed-distance drawing, text with stb_truetype and the Xbox, PlayStation, keyboard and mouse icons → `runtime/ui/glyphs/glyphs.c` and `glyph_draw.c`, adapted to MKW's RGBA sheet (PicFont) |
| [a55uka/nup](https://github.com/a55uka/nup) (a4840f0, `SPEC.md`) | MIT | The `.nup` dump package (exefs and romfs of a title in one file, each file raw or one zstd frame): its layout for `runtime/sdk/fs/nup.c` and `tools/prepare/nup.py`, written from the spec and real dumps; no code taken. Its `nup pack` made our test package from the 1.0.0 dump |
| [fab918/nxformat](https://github.com/fab918/nxformat) (208b815) | MIT | The `.nx` archive (files in blocks of at most 1 MiB, each raw or one zstd frame, so any range reads without decoding the whole file). Code: its C++17 reader `dist/nx_reader.hpp` as is in `runtime/third_party/nxformat/`, called from `runtime/sdk/fs/nx.cpp`; its `nxconvert pack-nup` made our test archive |
| [x-iso/FlexInput](https://github.com/x-iso/FlexInput) | MIT | Joy-Con 2 over Bluetooth LE in Rust. Taken (an idea and two bytes, 0905b2f): the input characteristic's report-rate descriptor `679d5510-…` and its `85 00` (`crates/joycon2/src/protocol.rs:126-131`), and asking Windows 11 for a throughput-optimized connection (`hub.rs:836-868`), in `runtime/input/switch2_win.c` |
| [TommyWabg/Switch2Connect](https://github.com/TommyWabg/Switch2Connect) | GPL-3.0 | Switch 2 controllers on Windows in Python. Idea only: the throughput-optimized connection against Windows' input lag (`src/controller.py:2119-2148`) |
| [seedds/mkw-neo](https://github.com/seedds/mkw-neo) | MIT | MKW on Apple hardware, from nxnative. Taken: the Switch 2 controllers' Bluetooth LE protocol, `dm/host/switch2_proto.h` and its test `dm/host/tests/t_switch2.c` (cfa62ddc), as `runtime/input/switch2_proto.h` and `tests/c/t_switch2.c` |
| [microsoft/windows-rs](https://github.com/microsoft/windows-rs) | MIT or Apache-2.0 | Rust bindings generated from the Windows metadata. Taken: the IID and vtable order of `IBluetoothLEDevice3`, missing from llvm-mingw's WinRT headers (`runtime/input/switch2_win.c`) |
| [seedds/nxnative](https://github.com/seedds/nxnative) | MIT | Runs native ARM code on Apple hardware (no CPU translation). **Also runs MKW** (2a6f9b3, 09/11/2026: NVN2 over Metal on Mac, tested per its README); C#, MSL and Python, more ideas and test cases than code. Its spin loop search is taken (through totk-pc-recomp: `tools/cpu/spin_waits.py`). A private fork by Will Faust runs MKW on Mac ([XDA, 27/10/2026](https://www.xda-developers.com/switch-2-mario-kart-world-running-on-mac-nintendo-probably-isnt-thrilled/)): code not published |
| [0xNikilite/oboromi](https://github.com/0xNikilite/oboromi), fork [jopdorp/kokoromi](https://github.com/jopdorp/kokoromi) | GPL-3.0 | A Switch 2 emulator in Rust: SM86 decoder generated from DocumentSASS, SM86→SPIR-V translation. Ideas only |
| [Djihads80/Eglantine](https://github.com/Djihads80/Eglantine), [Wael3rd/Eglantine-MKW](https://github.com/Wael3rd/Eglantine-MKW) | GPL-3.0 | Derived from eden-nx2; the MKW one targets 1.8.0. Reviewed @ e96917cd0e (01/11/2026). Taken as ideas: Switch 2's tick counter runs at 31.25 MHz (checked in our `sdk`, `GetSystemTickFrequency` at `sdk+0x145d00`) → `RT_TICK_HZ`; measuring how much of the sparse tiles ever gets written → `vk_sparse_written` |
| [Eglantine-community/Eglantine](https://github.com/Eglantine-community/Eglantine) | GPL-3.0 | Another Eglantine fork. Taken as an idea (PR #441, 16/11/2026, from its #553): a compute pass's storage-image stores are written back to guest memory, which carries the water's collision height map to the CPU physics → `vk_texture_compute_written` |
| [NotRexed/SkyboundTotk](https://github.com/NotRexed/SkyboundTotk) a4495f9 (31/10/2026) | MIT | Its frame interpolation (`a64-kit/gpu/render/rx_framegen.cpp`, WebGPU): quarter-size luma, block matching one vector per 16x16 pixels with a cost for long vectors, a 3x3 median, each made frame by moving both frames along the vector, and a display thread spreading them over the game's recent frame time. Ported to Vulkan in `runtime/gfx/vk/device/vk_framegen.c` and `runtime/shaders/fg_*.comp`, cited; the three-level search and the per-pixel choice of vector are ours. From Hookshot (GPL), ideas only: still pixels keep still, and every frame of a game frame is blitted at once and only the presents wait (its issue #1702) |
| [aappleby/smhasher](https://github.com/aappleby/smhasher) `src/MurmurHash3.cpp` | Public domain | The 32-bit MurmurHash3 (seed 0) that turns a course's file name into the CourseId the cups name, rewritten in C in `runtime/core/hacks/classic.c` (Classic Grand Prix and VS) |
| [Classic Grand Prix (No Intermissions!)](https://gamebanana.com/mods/736220) on GameBanana (MKW 1.8.0, 11/2026) | None stated: nothing taken | Only compared: its 8 cup files name the same 24 courses that `classic.c` derives from the game's own CourseGParam and CourseAreaGParam (Classic Grand Prix) |
| [Ca1nM/mario-kart-world-save-editor](https://github.com/Ca1nM/mario-kart-world-save-editor) (b12cbe6, 08/11/2026) | MIT | Editor of MKW 1.0.0's `account.sav` (Product.100): lists of drivers, vehicles and stickers, their states and the Special Cup. Its offsets and id list are taken (`runtime/core/hacks/unlock.c`, `unlock_ids.c`) |
| [NotRexed/Hookshot](https://github.com/NotRexed/Hookshot) (a598b9f, 08/11/2026) | GPL-3.0 | A Switch 2 Ocarina of Time recomp (Windows and Linux), **a fork of another, unpublished MKW recomp** whose `nx2/`, `host/` and `gpu/vk/` it carries whole: races at 45-55 fps on an RTX 4060 Ti. Its MKW is another version (build id `d00e4f39…`, SDK 21.4.0; ours `9e244438…`, 1.0.0). Ideas only |
| [0xD0GF00D/DocumentSASS](https://github.com/0xD0GF00D/DocumentSASS), [reoLantern/nvsass-disassembler](https://github.com/reoLantern/nvsass-disassembler) ([arXiv 2609.18662](https://arxiv.org/abs/2609.18662)) | MIT | The SASS encoding specification taken from `nvdisasm` itself (sm_80/sm_86/sm_89): fields per instruction and a decoder validated against nvdisasm on 3.78 M instructions. The base of the SM86 decoder (`sm86/decode`); `intercept.c`, `funnel.py`, `parse_sidl.py` and `sidl_*.py` copied unchanged into `sm86/spec`, which builds the spec on the player's machine. Needs CUDA 12.8's `nvdisasm` |
| [cloudcores/CuAssembler](https://github.com/cloudcores/CuAssembler) | MIT | An SM86 SASS assembler that learns the encoding from nvdisasm/binary pairs |
| [AnyPS5 #6528](https://github.com/boykopovar/AnyPS5/issues/6528) (PR #9356) | GPL-2.0 | Only the idea, for task/mesh as compute: without `VK_EXT_mesh_shader`, the mesh shader is compiled as a compute shader that writes vertices and triangles (the extra ones degenerate) to buffers, drawn by index with a generated vertex shader. Measured there: 15 of 20 games on a Radeon 680M without mesh shaders |
| [Dolphin, hybrid ubershader mode](https://dolphin-emu.org/blog/2017/07/30/ubershaders/) ([PR #5702](https://github.com/dolphin-emu/dolphin/pull/5702)) | GPL-2.0+ | Only the idea, for specialized shaders: draw with the generic shader while the specialized variant compiles in the background, and switch to it when ready |
| [Mesa environment variables](https://docs.mesa3d.org/envvars.html) | MIT (docs) | RADV RGP capture: `MESA_VK_TRACE=rgp`, `MESA_VK_TRACE_TRIGGER`, `MESA_VK_TRACE_FRAME` |
| [Switchbrew](https://switchbrew.org/w/index.php?title=Category%3AServices) | CC BY-SA | The "Switch 2: GSV / VI2 / Host1x / Nvkernel services" pages and the `[S2]` commands of `appletOE`. Only needed if something of the translated SDK calls them |
| [Axiom4912/Axiom](https://github.com/Axiom4912/Axiom), [hffgzktbcc-blip/ounce-core](https://github.com/hffgzktbcc-blip/ounce-core) | — / MIT | Discarded: a README without code; a research runtime that only runs Switch 1 homebrew |

### Reference copies (`scratchpad/ext/`, cloned again when missing)

| Copy | Commit | Where to look |
|---|---|---|
| `eden-nx2` | 50fa86bb | `NX2_NOTES.md` (blockers with cause and fix) before the code; `src/shader_recompiler/frontend/ampere/`, `src/video_core/host1x/nx2_video_decoder.cpp` |
| `totk` (totk-pc-recomp) | ae30a61 | `runtime/` (kernel, `nvn_procs.inc`), `tools/` (`spin_waits.py`, `shader_census.py`, `xlate/`) and `tools/vendor/ryujinx/src/Ryujinx.Graphics.Shader` (Ryujinx 1.3.2) |
| `totk-linux` (PR #60) | c626cda | `runtime/platform/linux.c`, `tools/build_game.py`, `tools/romfs_index.py` |
| `ryujinx` | a61a426 | `src/Ryujinx.Graphics.{Shader,Gpu,Texture,Vulkan}` |
| `nvsass-disassembler` | 28f9064 | `DocumentSASS/`, `parse_sidl.py`, `sass_decode.py` |

No public NVN2 documentation, decompilation, symbol map or exefs patches of MKW were found (30/10/2026).

## NVN2 against NVN1 (measured)

- NVN2's name table in `sdk` (0xb74c26-0xb79b76): 699 functions. Against the NVN1 one of the Switch 1 game
  (`totk-pc-recomp/runtime/nvn_procs.inc`, 537): **529 with the same name**, 170 new, 8 of NVN1 gone (debug markers
  and `*NvRm*NVX`). That the signature did not change in those 529 was a hypothesis, checked as the calls were
  translated.
- New ones by group: DLSS 54, `ProgramBuilder` 20 (programs are created with a builder), mesh/task
  (`DrawMeshTasks*`), ray tracing (`*AccelerationStructure*`, `DispatchRays*`), push constants, device messages, HDR
  in `Window*`, variable rate shading, exclusive scissor.
- `main` holds all 700 names (the loader's whole table); which ones it really calls is measured from the references
  of the translated code.
- DLSS: `main` creates its network with `nvnDlssNetwork*` and evaluates it with `nvnCommandBufferEvaluateDlss`. It
  sits behind an option, off by default; an AMD GPU has no host DLSS.

## Shaders

`romfs/Shader/`: 9.3 MB compressed, 55 MB uncompressed. Six `DriveRefreshUber.*.bfsha` (FSHA with a BNSH inside, four
in `Procedural`) and `Drive.Nin_Ounce_NVN2.release.sarc` (another bfsha and agl's `sharcb`, `BAHS`): the same
containers as the Switch 1 game. It is a closed set: all of them can be translated ahead of time. eden-nx2 dumped
1 712 distinct MKW programs at run time (row 88).

Game version: `romfs/System/Resource/ResourceSizeTable.Product.100…` points to 1.0.0 (no `control.nacp`). The latest
is 1.8.0 (09/09/2026).
