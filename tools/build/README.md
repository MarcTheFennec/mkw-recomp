# tools/build

Shortcuts for these flows: [`make help`](../../Makefile) at the repo root.

| Script | What it does | Usage |
|---|---|---|
| `icon.py` | Draws `mkw.exe`'s icon in code (our own chequered flag, no game art) into a `.ico` of 256, 48, 32 and 16 px; `build.py --target windows` calls it before the `runtime/platform/mkw.rc` resource; a `.png` name, the AppImage's 256 px icon; a `.c` name, `app_icon_set` with a 128 px copy that `build.py` builds into every target, so the window carries it (the Wayland taskbar shows it through `xdg_toplevel_icon`); `.svg`, the logo | `uv run tools/build/icon.py OUT.ico\|OUT.png\|OUT.svg` |
| `windeps.py` | Downloads to `build/win` what the Windows `.exe` needs: llvm-mingw 20261020 (`build/win/llvm-mingw`: its Linux build, or on a Windows host its Windows `.zip`) and the headers, libraries and DLLs of SDL3, Opus, libvpx, zstd, xxHash, Vulkan and VMA from MSYS2 UCRT64 (`build/win/deps`) with their licences (`deps/share/licenses`, for the release zip), each download checked against its sha256. SDL3 is built from source with CMake and Ninja (with libusb); on Windows, `cmake` or `ninja` missing from `PATH` are taken from MSYS2 UCRT64 (`C:/msys64/ucrt64/bin`, or `MSYS2_UCRT64_BIN`), and it stops before any download when they are not found | `uv run tools/build/windeps.py` (~1 min; 670 MB) |
| `dist.py` | Assembles the player's folder in `build/dist/<target>` after `build.py --dist`: `setup/` with a portable CPython (python-build-standalone) with numpy and lz4, zig 0.17.1 (for Windows also its C runtime, prebuilt in `setup/lib/crt/` so the setup links without building it: issue #2; made by zig's Linux build, or its Windows build on a Windows host), `glslc` from conda-forge and the sources the setup runs (`tools/prepare/prepare.py`); the start screen creates the desktop shortcut (`.desktop` on Linux, `.lnk` on Windows). Each download checked against its sha256 | `uv run tools/build/dist.py [--target linux\|windows] [--check]` |
| `sysroot.py` | Unpacks the Steam Runtime 3 "sniper" SDK sysroot (glibc 2.31) into `build/sysroot` and adds under its `usr/local`, built with clang against it: zstd 1.5.7 (static), the xxHash 0.8.3 header and SDL 3.4.18 (loads X11, Wayland and the sound servers at run time, so it needs only glibc). `build.py --sysroot build/sysroot` links the release's runtime against it: it starts on Debian 11, SteamOS and newer (built on Arch it needs Arch's glibc) | `uv run tools/build/sysroot.py` (~1.2 GB download the first time, ~3 min) |
| `release.py` | Packs a release from `build/dist/<target>`, the runtime and `setup/` only: the Linux AppImage (the runtime and SDL3, Opus and libvpx from the sysroot; `AppRun` runs a copy of `mkw` in `~/.local/share/mkw-recomp`, where the setup builds the game) or the Windows zip. Refuses a Linux runtime that needs a glibc newer than 2.31 and any file with the generated code's marker. `.github/workflows/release.yml` runs it on each `v*` tag | `uv run tools/build/release.py --target linux\|windows --version 0.1.0` |
| `notes.py` | A release's notes: `docs/release-template.md` (install, updating, known issues, reporting) filled with the version's section of `CHANGELOG.md` (summary, changes, what was tested). Refuses a section without them or with its tests still pending | `uv run tools/build/notes.py --version 0.1.3 [--out notes.md]` |
| `build.py` | Writes `build/obj/build.ninja` and builds with Ninja the C of `build/generated` (`build/obj/lib{main,subsdk0,sdk}.a`) and the runtime, and links `build/mkw`. Writes `build/obj/rt/modules.c` (the game tables of `runtime/core/boot/gamelib.h`: modules, function tables, ExeFS hashes, patch sets) and `defaults.c` (the copy's paths). `--dist`: the player's build in `build/dist/<target>`, the runtime (`mkw`, `mkw.exe`) and separately the game library (`mkwgame.so`, `mkwgame.dll`), which the runtime loads from its folder (objects in `build/obj-dist[-win]`; the library links with `cc`, without libstdc++). Parallel jobs come from a memory budget. The C goes through `ccache` when installed (`MKW_CCACHE=0` turns it off): moving folders changes every `-I` of the runtime and a comment in `runtime/cpu` rebuilds the whole game. `--frame-pointers`: runtime with frame pointers for `perf -g` stacks (where each thread blocks: `-e cs -g`). `--target windows`: `build/mkw.exe` with llvm-mingw, cross-built from Linux or on Windows itself (objects in `build/obj-win`; window, input, keyboard and menu through SDL3 as on Linux, `platform/win32.c` instead of `linux.c`). On a Windows host only `--target windows` is accepted; `ninja` comes from `PATH`, or else MSYS2 UCRT64 (`C:/msys64/ucrt64/bin`, or `MSYS2_UCRT64_BIN`); the archive and link commands go through response files (Windows' 32 K command-line limit). `--sysroot DIR`: Linux, compiled and linked against that sysroot (`sysroot.py`) | `uv run tools/build/build.py` (~7 min from scratch; seconds after) |

Before: `uv run python recomp/gen.py --exefs game/exefs --out build/generated --jobs 8 --patches recomp/patches-mkw.txt`
(generates the C).

Needs `clang`, `lld`, `ninja`, `sdl3`, `opus`, `libvpx`, `zstd`, `xxhash`, `vulkan-headers` and the VMA 3.4.0 header (on Arch
it is only in the AUR):

```sh
mkdir -p scratchpad/vma && curl -sSfL -o scratchpad/vma/vk_mem_alloc.h \
  https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/v3.4.0/include/vk_mem_alloc.h
# sha256 8487b7995ad3b263eb73bc5b9a77d71aa69b6bef5d58a715c02d2663afd81f1a
```

Run: `build/mkw --boot` (also `--selftest`, `--probe-loader`).

Without the game: `uv run tools/build/build.py --dist --runtime-only [--target windows]` builds only the runtime with
the start screen (`build/dist/<target>/mkw`), what a release ships and the CI checks. A release, as the workflow:

```sh
uv run tools/build/sysroot.py
uv run tools/build/build.py --dist --runtime-only --sysroot build/sysroot --out build/obj-sysroot
uv run tools/build/dist.py && uv run tools/build/release.py --version 0.1.0
```

## A packed dump

From a `.nup` or `.nx` file instead of the `exefs/` and `romfs/` folders: `build/mkw --unpack GAME.nup game exefs` (or
`GAME.nx`) gives `game/exefs` (40 MB), and the RomFS is read in place with `build/mkw --romfs GAME.nup` (`MKW_ROMFS`,
`romfs =` in `mkw.ini`). `tools/shaders/extract.py --romfs GAME.nup` reads a `.nup` too; for a `.nx`, unpack its shader
folders first (`build/mkw --unpack GAME.nx DIR romfs/Effect romfs/Layout romfs/Lib`).

## Windows

`build/mkw.exe` builds from Linux (a cross build) or on Windows itself, with the same llvm-mingw release and libraries
(`windeps.py`).

### From Linux

```sh
uv run tools/build/windeps.py
uv run tools/build/build.py --target windows   # ~7 min from scratch
tools/run/wine.sh build/mkw.exe --selftest
```

### On Windows

No Linux machine, WSL or Wine. It needs:

- [uv](https://docs.astral.sh/uv/) (it fetches the Python 3.14 of `pyproject.toml`) and git.
- CMake (tested with 4.4), for the SDL3 build of `windeps.py`: the installer from cmake.org, on `PATH`.
- Ninja from [MSYS2](https://www.msys2.org/) UCRT64: in an MSYS2 shell, `pacman -S mingw-w64-ucrt-x86_64-ninja`.
  `windeps.py` and `build.py` use the `ninja` on `PATH`, and else the one in `C:/msys64/ucrt64/bin`; set
  `MSYS2_UCRT64_BIN` when MSYS2 is installed elsewhere. Nothing else is needed from MSYS2: the compiler (llvm-mingw's
  Windows build) and the libraries come from `windeps.py`.

From the repo root, in PowerShell or `cmd`:

```bat
uv run tools/build/windeps.py
uv run tools/build/build.py --target windows
build\mkw.exe --selftest
```

The build takes ~7 min from scratch, seconds after.

`--target windows` is required (the default target is `linux`, refused on Windows). Generating the C and the shaders
(`recomp/gen.py`, `tools/shaders/`) works as on Linux. The release zip, with the steps of the release workflow's
`windows` job (which runs them on Linux):

```bat
uv run tools/build/build.py --target windows --dist --runtime-only
uv run tools/build/dist.py --target windows
uv run tools/build/release.py --target windows --version 0.1.0
```

To play on Windows, next to `mkw.exe`: the DLLs of `build/win/deps/bin` (except `libxxhash.dll`), `spirv/`, and the
game copy in `exefs/` and `romfs/` (or `--exefs`/`--romfs`). It needs a driver with Vulkan 1.3 (without
`VK_EXT_mesh_shader`, task and mesh shaders run as compute: `MKW_MESH_COMPUTE`) and a CPU with AVX2
(`-march=x86-64-v3`).

## Linux AArch64

The runtime and the game code build for 64-bit ARM Linux (ARMv8.2: Cortex-A76 class, Snapdragon 8 Gen 1 and newer),
cross-built from an x86-64 machine. `--arch aarch64` picks the CPU (default: this machine's); `sysroot.py --arch aarch64`
assembles a Debian 11 arm64 sysroot (glibc 2.31) in `build/sysroot-aarch64` from archive.debian.org, every package
checked against the pinned indexes. The `linux-aarch64` CI job builds the runtime this way (not a required check), and
the release workflow publishes its AppImage (`mkw-recomp-<version>-aarch64.AppImage`) since 0.1.9. Locally:

```sh
uv run tools/build/sysroot.py --arch aarch64
uv run tools/build/build.py --dist --runtime-only --arch aarch64 --sysroot build/sysroot-aarch64 --out build/obj-sysroot-aarch64
uv run tools/build/dist.py --target linux-aarch64 && uv run tools/build/release.py --target linux-aarch64 --version 0.1.0
```

On the device the setup works as on x86-64: the package's zig builds the game code, NVIDIA's AArch64 ("sbsa") `nvdisasm`
and `ptxas` build the SM86 spec. Tested on an AYN Odin 2 (Snapdragon 8 Gen 2, Turnip, SteamOS ARM); rendering there
also needs the Adreno workarounds in `runtime/gfx/vk`, which come separately.
