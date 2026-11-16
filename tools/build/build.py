"""Build the game: compile the generated C (build/generated, from recomp/gen.py) and the runtime, link build/mkw (or
build/mkw.exe with --target windows).

Why: the generated code is ~0.9 GB of C in ~355 chunks; Ninja recompiles only what changed and runs the chunks in
parallel. Each job can take several hundred MB, so the job count comes from a memory budget, not the core count.

Flags, libraries and the module glue follow totk-pc-recomp tools/build_game.py on its Linux port branch (linux-port
@ eccf4dd, MIT): GEN_FLAGS, RT_FLAGS, LINK_FLAGS, LINK_LIBS and module_glue(). build/obj/rt/modules.c carries the game
code's tables (runtime/core/boot/gamelib.h: module layout, function tables, the SHA-256 of each ExeFS file read now from the
game copy, patch sets); build/obj/rt/defaults.c the default game paths; build/obj/rt/i18n/ the menu texts
of every language, from i18n/*.po (tools/i18n/catalog.py).

--target windows builds build/mkw.exe with llvm-mingw and the MSYS2 UCRT64 libraries that tools/build/windeps.py puts
in build/win (totk-pc-recomp's --toolchain build: its LLVM_LINK_LIBS), cross-compiled from Linux or on Windows itself,
where it is the only target and ninja comes from PATH or else MSYS2 UCRT64 (C:/msys64/ucrt64/bin, MSYS2_UCRT64_BIN).
Objects go to build/obj-win. The window, input, keyboard and menu are SDL3 on both systems; platform/win32.c replaces
platform/linux.c.

Usage: uv run tools/build/build.py [--target linux|windows] [--gen build/generated] [--exefs DIR] [--romfs DIR]
       [--jobs N] [--frame-pointers] [--dry-run]
Heavy (all cores, several GB): our runs go through tools/run/job.sh.
"""

import argparse
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "i18n"))
import catalog  # noqa: E402  (tools/i18n: the text catalogs from i18n/*.po)

ROOT = Path(__file__).resolve().parents[2]
RUNTIME = ROOT / "runtime"
GAME = Path(os.environ.get("MKW_GAME", ROOT / "game"))
WIN = ROOT / "build/win"  # tools/build/windeps.py
# The CPU the game and runtime are built for. x86-64: AVX2 (x86-64-v3). AArch64: ARMv8.2 with the extensions of every
# Cortex-A76-class core or newer (Snapdragon 8cx/8 Gen 1+, Raspberry Pi 5, Apple M): LSE atomics, FP16, RCpc, dot
# product, CRC32. The lifted code also leaves x18 alone there: kern_ap_stub (runtime/core/kernel/kernel_preempt.c)
# returns through it.
ARCH_FLAGS = {"x86_64": ["-march=x86-64-v3"], "aarch64": ["-march=armv8.2-a+fp16+rcpc+dotprod+crc"]}
GEN_ARCH_FLAGS = {"x86_64": [], "aarch64": ["-ffixed-x18"]}
HOST_ARCH = {"amd64": "x86_64", "x64": "x86_64", "arm64": "aarch64"}.get(platform.machine().lower(),
                                                                          platform.machine().lower())  # fmt: skip
ARCH = HOST_ARCH if HOST_ARCH in ARCH_FLAGS else "x86_64"
GEN_FLAGS = ["-O1", "-fno-strict-aliasing", "-Wno-return-type", "-Wno-unused-label", "-fvisibility=hidden",
             *ARCH_FLAGS[ARCH], *GEN_ARCH_FLAGS[ARCH]]  # fmt: skip
RT_FLAGS = ["-O2", "-g", "-Wall", "-fno-strict-aliasing", *ARCH_FLAGS[ARCH]]
sys.path.insert(0, str(ROOT / "recomp"))
from versions import detect as detect_version  # noqa: E402  (the game version of a dump: patches, RtGame.version)


def gen_modules(gen: Path) -> list[str]:
    """The modules the generator wrote, in layout order (1.0.0: main, subsdk0, sdk; 1.8.0 adds subsdk1)."""
    return [m["name"] for m in json.loads((gen / "layout.json").read_text())]


def set_arch(arch: str) -> None:
    """Builds for arch from here on (GEN_FLAGS, RT_FLAGS and game_id() follow; the lists change in place)."""
    global ARCH
    ARCH = arch
    GEN_FLAGS[5:] = [*ARCH_FLAGS[arch], *GEN_ARCH_FLAGS[arch]]
    RT_FLAGS[4:] = ARCH_FLAGS[arch]


# Built on Windows itself (not cross-compiled from Linux): Ninja runs each command with CreateProcess, no shell, and
# Windows caps a command line at 32 K characters; see windows_host_rules() and windows_host_ninja().
HOST_WINDOWS = os.name == "nt"
MSYS2_UCRT64_BIN = Path(os.environ.get("MSYS2_UCRT64_BIN", "C:/msys64/ucrt64/bin"))  # ninja, glslc, spirv-val


@dataclass(frozen=True)
class Target:
    cc: str
    cxx: str
    ar: str
    exe: Path
    rt_flags: list[str]
    link_flags: list[str]
    link_libs: list[str]
    vma: Path  # the directory with vk_mem_alloc.h
    windres: str = ""  # Windows: compiles runtime/platform/mkw.rc (the exe's manifest)


LINUX = Target(
    "clang", "clang++", "llvm-ar", ROOT / "build/mkw",
    ["-D_GNU_SOURCE"],
    # -no-pie: the function tables (hundreds of thousands of pointers) need no load-time relocation.
    ["-fuse-ld=lld", "-no-pie", "-Wl,-z,noexecstack", "-Wl,-Map=$builddir/mkw.map"],
    ["-lvulkan", "-lSDL3", "-lopus", "-lvpx", "-lzstd", "-l:libstdc++.a", "-lm", "-lpthread", "-ldl"],
    ROOT / "scratchpad/vma",  # vk_mem_alloc.h 3.4.0 (AUR only on Arch); fetched as in tools/build/README.md
)  # fmt: skip
WINDOWS = Target(
    str(WIN / "llvm-mingw/bin/x86_64-w64-mingw32-clang"), str(WIN / "llvm-mingw/bin/x86_64-w64-mingw32-clang++"),
    str(WIN / "llvm-mingw/bin/llvm-ar"), ROOT / "build/mkw.exe",
    [f"-I{WIN / 'deps/include'}"],
    # 16 MiB main-thread stack, as mkw; the map as on Linux.
    [f"-L{WIN / 'deps/lib'}", "-fuse-ld=lld", "-Wl,--stack,16777216", "-Wl,--Map=$builddir/mkw.map"],
    ["-lpsapi", "-lbcrypt", "-ladvapi32", "-lws2_32", "-liphlpapi", "-luser32", "-lole32", "-luuid", "-lshell32", "-lgdi32", "-ldwmapi", "-lruntimeobject", "-lwinmm", "-lvulkan-1", "-lSDL3", "-lopus",
     "-l:libvpx.a", "-l:libzstd.a", "-l:libc++.a", "-l:libc++abi.a", "-l:libunwind.a", "-l:libwinpthread.a"],
    WIN / "deps/include/vma",
    str(WIN / "llvm-mingw/bin/x86_64-w64-mingw32-windres"),
)  # fmt: skip


def windows_memory_gb() -> tuple[float, float]:
    """Windows' total and available physical memory in GiB (GlobalMemoryStatusEx, MEMORYSTATUSEX); OSError if the
    call fails. Shared with tools/prepare/machine.py mem_available_gb."""
    import ctypes

    class Status(ctypes.Structure):  # MEMORYSTATUSEX
        _fields_ = [("length", ctypes.c_uint32), ("load", ctypes.c_uint32), ("total", ctypes.c_uint64),
                    ("avail", ctypes.c_uint64), ("rest", ctypes.c_uint64 * 5)]  # fmt: skip

    st = Status(length=ctypes.sizeof(Status))
    if not ctypes.WinDLL("kernel32", use_last_error=True).GlobalMemoryStatusEx(ctypes.byref(st)):
        raise ctypes.WinError(ctypes.get_last_error())
    return st.total / 2**30, st.avail / 2**30


def mem_total_gb() -> float:
    """The machine's memory in GiB, for the job counts (build.py --job-mem-gb, prepare.py fit). Windows has no
    /proc/meminfo."""
    if os.name == "nt":
        return windows_memory_gb()[0]
    with open("/proc/meminfo") as f:
        for line in f:
            if line.startswith("MemTotal:"):
                return int(line.split()[1]) / 1024**2
    raise RuntimeError("MemTotal missing from /proc/meminfo")


def runtime_sources(target: Target) -> list[Path]:
    """Every runtime .c/.cpp of the target. Linux: no Win32 file (*_win.*, platform/win32*.c). Windows: no
    platform/linux*.c."""
    srcs = [*RUNTIME.rglob("*.c"), *RUNTIME.rglob("*.cpp")]
    if target is LINUX:
        return sorted(p for p in srcs if not p.stem.endswith("_win") and not p.stem.startswith("win32"))
    return sorted(p for p in srcs if not p.stem.startswith("linux"))


def c_string(s: str) -> str:
    return json.dumps(s)


def game_id() -> str:
    """RT_GAME_ID: the headers the generated code is compiled against and its flags. A game code library built with
    another id is not loaded (runtime/core/boot/gamelib.c)."""
    h = hashlib.sha256(" ".join(GEN_FLAGS).encode())
    for p in sorted([*(RUNTIME / "cpu").glob("*.h"), RUNTIME / "core/boot/gamelib.h"]):
        # without CRs: Git for Windows checks out with CRLF (core.autocrlf=true), and a runtime built from such a clone
        # refused the release's mkwgame.dll of the same headers as "built for another version" (16/11/2026)
        h.update(p.read_bytes().replace(b"\r\n", b"\n"))
    return h.hexdigest()[:16]


def module_glue(gen: Path, exefs: Path, library: bool) -> str:
    """modules.c: the game code's tables (runtime/core/boot/gamelib.h): layout, function tables, the SHA-256 of each ExeFS
    file read now from the game copy, the patch sets of the generated patches.h, and the data the generated code reads
    on every call. library: the entry point of mkwgame.so / mkwgame.dll instead of rt_game_builtin."""
    layout = [m["name"] for m in json.loads((gen / "layout.json").read_text())]
    out = ["// Game code tables written by tools/build/build.py; do not edit.", '#include "gamelib.h"',
           '#include "layout.h"', '#if __has_include("patches.h")', '#include "patches.h"', "#endif", ""]  # fmt: skip
    for name in layout:
        out += [f"extern const uint64_t {name}_fn_addrs[];", f"extern const GuestFn {name}_fn_ptrs[];",
                f"extern const uint64_t {name}_fn_count;"]  # fmt: skip
    out += ["", "static const RtGameModule modules[] = {"]
    for name in layout:
        u = name.upper()
        out.append(f'    {{"{name}", MOD_{u}_BASE, MOD_{u}_SIZE, {name}_fn_addrs, {name}_fn_ptrs, &{name}_fn_count}},')
    out += ["};", "", "static const RtFileHash exefs_sha256[] = {"]
    files = sorted(p for p in exefs.iterdir() if p.is_file() and not p.name.startswith("."))
    out += [f"    {{{c_string(p.name)}, {c_string(hashlib.sha256(p.read_bytes()).hexdigest())}}}," for p in files]
    out += ["};", "", "#ifdef RT_PATCH_COUNT", "static const RtPatchSet patch_sets[] = {",
            "#define X(i, n, e, d) {#n, e, d},", "    RT_PATCH_SETS(X)", "#undef X", "};", "#else",
            "#define RT_PATCH_COUNT 0", "#define RT_PATCH_PAGE 0", "static const RtPatchSet *const patch_sets = 0;",
            "#endif",
            # the enabled code mods' bytes (gen.py --image), for the loader to write into each module's .text
            "#if defined(RT_IMAGE_PATCH_COUNT) && RT_IMAGE_PATCH_COUNT",
            "static const RtImagePatch image_patches[] = {",
            "#define X(m, off, b, n) {m, off, (const uint8_t *)b, n},", "    RT_IMAGE_PATCHES(X)", "#undef X", "};",
            "#else", "#undef RT_IMAGE_PATCH_COUNT", "#define RT_IMAGE_PATCH_COUNT 0",
            "static const RtImagePatch *const image_patches = 0;", "#endif", "",
            # RtGame.version: recomp/versions.py's number for the dump (100: 1.0.0, 180: 1.8.0)
            "volatile int g_fnprof;", "uint8_t rt_patch_on[RT_MAX_PATCH_SETS];", "",
            "static const RtGame game = {RT_GAME_ABI, RT_GAME_ID, modules, sizeof modules / sizeof *modules,",
            "                            exefs_sha256, sizeof exefs_sha256 / sizeof *exefs_sha256, patch_sets,",
            f"                            RT_PATCH_COUNT, RT_PATCH_PAGE, {detect_version(exefs).number}, &g_fnprof, rt_patch_on,",
            "                            image_patches, RT_IMAGE_PATCH_COUNT};", "",
            "RT_GAME_EXPORT const RtGame *mkwgame_tables(void) { return &game; }" if library
            else "const RtGame *rt_game_builtin(void) { return &game; }", ""]  # fmt: skip
    return "\n".join(out)


def port_version() -> str:
    """The release tag without its v (0.1.0) or, between tags, git describe's tag-commits-hash; the commit alone in a
    clone without tags, "dev" outside git (the source tarball of a release says nothing)."""
    r = subprocess.run(["git", "-C", str(ROOT), "describe", "--tags", "--always", "--dirty"],
                       capture_output=True, text=True, check=False)  # fmt: skip
    return r.stdout.strip().removeprefix("v") if r.returncode == 0 and r.stdout.strip() else "dev"


def defaults_glue(exefs: Path, romfs: Path, dist: bool) -> str:
    """defaults.c: the executable's own defaults, the game copy's paths (none in the player's build: the folders next
    to the executable, main.c portable_paths) and the port's version. The player's build gets rt_game_builtin here:
    it has no linked game code, gamelib.c loads the library."""
    out = ["// Defaults written by tools/build/build.py; do not edit.", '#include "gamelib.h"', '#include "host.h"', ""]
    for name, path in (("exefs", exefs), ("romfs", romfs), ("profile", ROOT / "build/profile")):
        out.append(f"const char rt_default_{name}[] = {c_string('' if dist else str(path.resolve()))};")
    out += [f"const char rt_port_version[] = {c_string(port_version())};", ""]
    if dist:
        out += ["const RtGame *rt_game_builtin(void) { return 0; }", ""]
    return "\n".join(out)


# What the game code library calls in the runtime (runtime/cpu/cpu.h): the executable exports them.
RT_EXPORTS = ("rt_dispatch", "rt_sj_owner", "rt_svc", "rt_udf", "rt_ticks", "rt_spin_gate", "fnprof_hit")


def ninja_file(target: Target, gen: Path, out: Path, rt_extra: list[str], dist: Path | None,
               runtime_only: bool = False, ld_extra: list[str] | None = None,
               sys_flags: list[str] | None = None) -> str:  # fmt: skip
    """dist: the player's build into that folder, the runtime (mkw, mkw.exe) and the game code apart (mkwgame.so,
    mkwgame.dll); else build/mkw with the game code linked in. runtime_only (with dist): the runtime alone, what a
    release ships and the CI builds, without the game's generated C."""
    inc = " ".join(f"-I{d}" for d in sorted({RUNTIME} | {p.parent for p in RUNTIME.rglob("*.h")}))
    gid = f'-DRT_GAME_ID=\\"{game_id()}\\"'
    pic = ["-fPIC"] if dist and target is LINUX else []
    link_flags = list(target.link_flags) + (ld_extra or [])
    if dist and target is LINUX:
        link_flags += [f"-Wl,--export-dynamic-symbol={s}" for s in RT_EXPORTS]
    elif dist:  # the exports (a .def file lld reads as an input) and the import library the game code links against
        out.mkdir(parents=True, exist_ok=True)
        (out / "mkw.def").write_text("EXPORTS\n" + "".join(f"    {s}\n" for s in RT_EXPORTS))
        # GUI subsystem: started from Explorer or a shortcut, no console window opens (main() stays the entry)
        link_flags += [str(out / "mkw.def"), f"-Wl,--out-implib,{out / 'libmkw.dll.a'}", "-Wl,--subsystem,windows"]
    # ccache before the C compilers (MKW_CCACHE=0 leaves it out): a folder move changes every runtime command's -I list
    # and a comment in runtime/cpu recompiles all the game code; ninja rebuilds both, ccache hands back the objects
    # (07/11/2026: ~12 min per step of a bisection). Not for C++: menu_look.cpp #embeds a file ccache may not track.
    cc = f"ccache {target.cc}" if os.environ.get("MKW_CCACHE", "1") != "0" and shutil.which("ccache") else target.cc
    lines = [
        f"builddir = {out}",
        f"genflags = {' '.join(GEN_FLAGS + pic + (sys_flags or []))} -I{RUNTIME / 'cpu'}",
        f"rtflags = {' '.join(RT_FLAGS + target.rt_flags + rt_extra)} {gid} {inc} -I{gen} -I{out / 'rt'} -I{target.vma}",
        f"tabflags = {' '.join(GEN_FLAGS + pic + (sys_flags or []))} {gid} {inc} -I{gen}",
        f"ldflags = {' '.join(link_flags)}",
        (
            f"rule cc\n  command = {cc} $genflags -MD -MF $out.d -c $in -o $out\n  depfile = $out.d\n  deps = gcc"
            "\n  description = cc $in"
        ),
        (
            f"rule rtcc\n  command = {cc} -std=gnu11 $rtflags -MD -MF $out.d -c $in -o $out\n  depfile = $out.d"
            "\n  deps = gcc\n  description = rt $in"
        ),
        (
            f"rule rtcxx\n  command = {target.cxx} -std=c++17 $rtflags -MD -MF $out.d -c $in -o $out\n  depfile = $out.d"
            "\n  deps = gcc\n  description = rt $in"
        ),
        (
            f"rule tabcc\n  command = {cc} -std=gnu11 $tabflags -MD -MF $out.d -c $in -o $out\n"
            "  depfile = $out.d\n  deps = gcc\n  description = cc $in"
        ),
        *(
            windows_host_rules(target)
            if HOST_WINDOWS
            else [
                f"rule ar\n  command = rm -f $out && {target.ar} rcs $out $in\n  description = ar $out",
                (
                    f"rule solink\n  command = {' '.join([target.cc, *(sys_flags or [])])} -shared -fuse-ld=lld -o $out $in $solibs"
                    "\n  description = link $out"
                ),
                (
                    f"rule link\n  command = {target.cxx} $ldflags -o $out $in {' '.join(target.link_libs)}"
                    "\n  description = link $out"
                ),
            ]
        ),  # fmt: skip
        f"rule rc\n  command = {target.windres} -O coff -I {out / 'rt/platform'} $in -o $out\n  description = rc $in",
        f"rule icon\n  command = {sys.executable} $in $out\n  description = icon $out",
    ]
    libs = []
    for mod in () if runtime_only else gen_modules(gen):
        objs = []
        for src in sorted((gen / mod).glob("*.c")):
            obj = out / mod / (src.stem + ".o")
            objs.append(str(obj))
            lines.append(f"build {obj}: cc {src}")
        lib = out / f"lib{mod}.a"
        libs.append(str(lib))
        lines.append(f"build {lib}: ar {' '.join(objs)}")
    rt_objs = []
    rt_srcs = [*runtime_sources(target), out / "rt/defaults.c", *sorted((out / "rt/i18n").glob("ui_text_*.c"))]
    rt_srcs.append(out / "rt/app_icon.c")   # the window's icon, drawn by tools/build/icon.py (never committed)
    lines.append(f"build {out / 'rt/app_icon.c'}: icon {ROOT / 'tools/build/icon.py'}")
    rt_srcs += [] if dist else [out / "rt/modules.c"]
    for src in rt_srcs:
        rel = src.relative_to(RUNTIME) if src.is_relative_to(RUNTIME) else Path(src.name)
        obj = out / "rt" / rel.with_suffix(".o")
        rt_objs.append(str(obj))
        lines.append(f"build {obj}: {'rtcxx' if src.suffix == '.cpp' else 'rtcc'} {src}")
    if target.windres:  # windres writes no depfile: the manifest and the icon it embeds are listed by hand
        rc = RUNTIME / "platform/mkw.rc"
        ico = out / "rt/platform/mkw.ico"  # drawn by tools/build/icon.py, never committed
        rt_objs.append(str(out / "rt/platform/mkw_rc.o"))
        lines.append(f"build {ico}: icon {ROOT / 'tools/build/icon.py'}")
        lines.append(f"build {rt_objs[-1]}: rc {rc} | {rc.with_suffix('.manifest')} {ico}")
    if not dist:
        lines.append(f"build {target.exe}: link {' '.join(rt_objs)} {' '.join(libs)}")
        return "\n".join(lines) + "\n"
    exe = dist / target.exe.name
    tab = out / "rt/modules.o"
    if runtime_only:
        lines.append(f"build {exe}{f' | {out}/libmkw.dll.a' if target is WINDOWS else ''}: link {' '.join(rt_objs)}")
        return "\n".join(lines) + "\n"
    lines.append(f"build {tab}: tabcc {out / 'rt/modules.c'}")
    if target is LINUX:
        lines.append(f"build {exe}: link {' '.join(rt_objs)}")
        lines.append(f"build {dist / 'mkwgame.so'}: solink {tab} {' '.join(libs)}")
        lines.append("  solibs = -Wl,-z,noexecstack")
    else:
        implib = out / "libmkw.dll.a"
        lines.append(f"build {exe} | {implib}: link {' '.join(rt_objs)}")
        lines.append(f"build {dist / 'mkwgame.dll'}: solink {tab} {' '.join(libs)} | {implib}")
        lines.append(f"  solibs = {implib}")
    return "\n".join(lines) + "\n"


def windows_host_rules(target: Target) -> list[str]:
    """ar, solink and link when Ninja runs on Windows. No shell: "rm -f $out && ..." would hand rm the "&&", the
    archiver and every object as files to delete. The object lists go through response files (the game's archive and
    the link pass the 32 K command-line limit); clang reads a response file the GNU way by default, where a backslash
    escapes, so the links ask for Windows quoting. llvm-ar replaces members by name, so without the rm a chunk that
    disappears stays in its archive until build/obj-win is cleaned."""
    rsp = "\n  rspfile = $out.rsp\n  rspfile_content = $in"
    return [
        f"rule ar\n  command = {target.ar} rcs $out @$out.rsp{rsp}\n  description = ar $out",
        (
            f"rule solink\n  command = {target.cc} -shared -fuse-ld=lld --rsp-quoting=windows -o $out @$out.rsp"
            f" $solibs{rsp}\n  description = link $out"
        ),
        (
            f"rule link\n  command = {target.cxx} $ldflags --rsp-quoting=windows -o $out @$out.rsp"
            f" {' '.join(target.link_libs)}{rsp}\n  description = link $out"
        ),
    ]


def windows_host_ninja(text: str) -> str:
    """Ninja reads the ":" of a drive letter in a build line as the end of its outputs: escape it ($:)."""
    drive = re.compile(r"(?<![$\w])([A-Za-z]):(?=[\\/])")
    return "\n".join(drive.sub(r"\1$:", line) if line.startswith("build ") else line for line in text.split("\n"))


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--gen", type=Path, default=ROOT / "build/generated")
    ap.add_argument("--target", choices=("linux", "windows"), default="linux")
    ap.add_argument("--arch", choices=tuple(ARCH_FLAGS), default=ARCH, help="Linux: the CPU (default: this machine's)")
    ap.add_argument("--out", type=Path, help="object directory (default build/obj, build/obj-win for windows)")
    ap.add_argument(
        "--dist", action="store_true",
        help="the player's build: runtime and game code library apart, in build/dist/<target> (objects in obj-dist)",
    )  # fmt: skip
    ap.add_argument(
        "--runtime-only", action="store_true",
        help="with --dist: the runtime alone (the release and the CI), no generated C or game copy needed",
    )  # fmt: skip
    ap.add_argument("--exefs", type=Path, default=GAME / "exefs")
    ap.add_argument("--romfs", type=Path, default=GAME / "romfs")
    ap.add_argument("--jobs", type=int, default=0, help="parallel compiles (default: from the memory budget)")
    # 0.5: measured peaks 13/11/2026, 12 jobs at once: the largest game chunks 266-300 MB, main_table.c 409 MB, zig cc
    # with the setup's flags 291 MB, the runtime's C++ (imgui.cpp) 199 MB. 1.0 left a third of a 12-thread PC idle.
    ap.add_argument("--job-mem-gb", type=float, default=0.5, help="memory one job may take (a main chunk: ~0.3 GB)")
    ap.add_argument(
        "--sysroot", type=Path,
        help="Linux: compile and link against this sysroot (tools/build/sysroot.py: glibc 2.31, the release)",
    )  # fmt: skip
    ap.add_argument("--dry-run", action="store_true", help="write build.ninja and show what would run")
    ap.add_argument(
        "--frame-pointers",
        action="store_true",
        help="runtime with frame pointers: perf call graphs (-g) of where threads block, e.g. -e cs",
    )
    args = ap.parse_args()
    # Absolute, not resolved (game/ is a link): ninja puts $in in each command and ccache hashes the input's path, so a
    # relative --gen next to the absolute default rebuilt all 623 objects for real on every switch (16/11/2026).
    for name in ("gen", "exefs", "romfs", "out"):
        if getattr(args, name):
            setattr(args, name, getattr(args, name).absolute())
    if args.runtime_only and not args.dist:
        ap.error("--runtime-only needs --dist")
    if HOST_WINDOWS and args.target != "windows":
        ap.error("on Windows only --target windows builds (mkw.exe)")
    target = WINDOWS if args.target == "windows" else LINUX
    if args.arch != "x86_64" and target is WINDOWS:
        ap.error("--target windows is x86_64 only")
    set_arch(args.arch)
    if args.sysroot and target is WINDOWS:
        ap.error("--sysroot is for Linux")
    # zstd and xxhash.h of the sysroot sit in its usr/local (tools/build/sysroot.py), whose include clang already reads
    sys_flags = [f"--target={ARCH}-linux-gnu", f"--sysroot={args.sysroot.resolve()}"] if args.sysroot else []
    sys_ld = [*sys_flags, f"-L{args.sysroot.resolve()}/usr/local/lib"] if args.sysroot else []
    cross = ARCH != HOST_ARCH and target is LINUX  # another CPU's build: its own dist and object folders
    dist = ROOT / "build/dist" / (args.target + (f"-{ARCH}" if cross else "")) if args.dist else None
    args.out = args.out or ROOT / ("build/obj" + ("-dist" if dist else "") + ("-win" if target is WINDOWS else "")
                                   + (f"-{ARCH}" if cross else ""))  # fmt: skip
    if HOST_WINDOWS and not shutil.which("ninja") and (MSYS2_UCRT64_BIN / "ninja.exe").is_file():
        # at the end, as windeps.py does: what PATH already has stays first
        os.environ["PATH"] = f"{os.environ['PATH']}{os.pathsep}{MSYS2_UCRT64_BIN}"
    for tool in (target.cc, target.cxx, "ninja", target.ar):
        if not shutil.which(tool):
            sys.exit(f"{tool} not found" + (" (uv run tools/build/windeps.py)" if target is WINDOWS else ""))
    if not (target.vma / "vk_mem_alloc.h").is_file():
        sys.exit(f"{target.vma}/vk_mem_alloc.h missing (tools/build/README.md)")
    jobs = args.jobs or max(1, min(os.cpu_count() or 1, int(mem_total_gb() * 0.6 / args.job_mem_gb)))
    (args.out / "rt").mkdir(parents=True, exist_ok=True)
    glue = [(args.out / "rt/defaults.c", defaults_glue(args.exefs, args.romfs, dist is not None))]
    if not args.runtime_only:
        glue.append((args.out / "rt/modules.c", module_glue(args.gen, args.exefs, dist is not None)))
    for path, text in glue:
        if not path.exists() or path.read_text() != text:  # an unchanged file keeps its mtime: no rebuild
            path.write_text(text)
    catalog.generate(args.out / "rt/i18n")  # the menu texts of every language (i18n/*.po)
    # which headers the library is built with, for the start screen (runtime/core/boot/gamelib.c rt_game_present)
    if dist:
        dist.mkdir(parents=True, exist_ok=True)
        (dist / "mkwgame.id").write_text(game_id())
    fp = ["-fno-omit-frame-pointer", "-mno-omit-leaf-frame-pointer"] if args.frame_pointers else []
    ninja = (
        # the lifted code and its library take the sysroot only for another CPU: an x86-64 release's stay as they were
        ninja_file(
            target, args.gen, args.out, fp + sys_flags, dist, args.runtime_only, sys_ld, sys_flags if cross else None
        )
    )
    (args.out / "build.ninja").write_text(windows_host_ninja(ninja) if HOST_WINDOWS else ninja)
    if target is LINUX and not dist:
        write_compile_db(args.out / "build.ninja")
    cmd = ["ninja", "-f", str(args.out / "build.ninja"), "-j", str(jobs)] + (["-n"] if args.dry_run else [])
    print(f"{' '.join(cmd)}  ({jobs} jobs)")
    sys.exit(subprocess.run(cmd, check=False).returncode)


def write_compile_db(ninja: Path) -> None:
    """build/compile_commands.json for clangd (diagnostics right after an edit, no build): runtime only, the generated
    chunks (~0.9 GB) would keep its background index busy. ccache is dropped: clangd takes argv[0] as the compiler."""
    out = subprocess.run(["ninja", "-f", str(ninja), "-t", "compdb", "rtcc", "rtcxx"],
                         capture_output=True, text=True, check=True).stdout  # fmt: skip
    entries = json.loads(out)
    for entry in entries:
        entry["command"] = entry["command"].removeprefix("ccache ")
    db = ROOT / "build/compile_commands.json"
    text = json.dumps(entries, indent=1)
    if not db.exists() or db.read_text() != text:
        db.write_text(text)


if __name__ == "__main__":
    main()
