"""The machine the setup runs on, for prepare.py: the package's tools (setup/ next to the executable) or the system's,
the tool processes tied to this one, and the memory that sizes the parallel jobs.

Why apart: prepare.py holds the steps; this is what they run with. Not run on its own.
"""

import json
import os
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/build"))
import build  # (mem_total_gb, windows_memory_gb)


def emit(*words: object) -> None:
    """One event for the start screen (stdout) and the same line for logs/setup.log (stderr)."""
    print(*words, flush=True)
    print(*words, file=sys.stderr)


class Failed(Exception):
    """A step that cannot go on: its message is for the player. code names it for the start screen, which shows its own
    translated text for it with detail inside (launcher.cpp setup_error_text); without a detail, the message."""

    def __init__(self, message: str, code: str = "failed", detail: str | None = None) -> None:
        super().__init__(message)
        self.code, self.detail = code, message if detail is None else detail


# ---- the package's tools (setup/ next to the executable), else the system's
def package_dir(game: Path) -> Path:
    # In a release the sources live in setup/src. Tools stay there when the player chooses another installation
    # folder; resolving this file also follows the AppImage's setup symlink into its current mount.
    bundled = Path(__file__).resolve().parents[3]
    if (bundled / "src/tools/prepare/machine.py").is_file():
        return bundled
    return game / "setup"


def default_cc(game: Path, target: str) -> list[str]:
    zig = package_dir(game) / "zig" / ("zig.exe" if os.name == "nt" else "zig")
    if zig.is_file():
        # zig finds its lib/ from the path it reads as its own executable (/proc/self/exe); when that path is not the
        # package's (a launcher or sandbox that copies or wraps the binary) every zig cc fails with "unable to find zig
        # installation directory" (reported on 0.1.2). ZIG_LIB_DIR names it; the tools below inherit it.
        lib = zig.parent / "lib"
        if not (lib / "std/std.zig").is_file():
            raise Failed(f"{lib} is incomplete: extract the release again")
        os.environ["ZIG_LIB_DIR"] = str(lib)
        use_own_zig_cache(game)
        return [str(zig), "cc", "-target", "x86_64-windows-gnu" if target == "windows" else f"{build.ARCH}-linux-gnu"]
    return ["clang"]


ZIG_CACHE = "zig-cache"  # in <game>/setup-work, kept by the cleanup step like obj/


def use_own_zig_cache(game: Path) -> None:
    """zig's caches (global and local) inside the game folder, setup-work/zig-cache, unless the player set
    ZIG_GLOBAL_CACHE_DIR. In the user's cache folder zig's rename from its tmp/ into o/ failed with CrossDevice on a
    Linux whose /home is one file system, and a one-file rename tried from Python there worked (#439, 16/11/2026: the
    player's own ZIG_GLOBAL_CACHE_DIR was what made it build). The game folder is where the setup writes everything
    else."""
    if os.environ.get("ZIG_GLOBAL_CACHE_DIR"):
        return
    own = game / "setup-work" / ZIG_CACHE
    own.mkdir(parents=True, exist_ok=True)
    os.environ["ZIG_GLOBAL_CACHE_DIR"] = os.environ["ZIG_LOCAL_CACHE_DIR"] = str(own)


def crt_spec(game: Path, cc: list[str]) -> Path | None:
    """The package's prebuilt Windows C runtime (setup/lib/crt/link.json, tools/build/dist.py windows_crt), or None
    without it (a source checkout, another compiler): then zig cc links and builds its libc itself, the build that never
    ended on many players' Windows PCs (issue #2)."""
    spec = package_dir(game) / "lib/crt/link.json"
    return spec if Path(cc[0]).stem == "zig" and spec.is_file() else None


def lld_link(zig: str, spec: Path, out: Path, inputs: list[str], *flags: str) -> list[str]:
    """A DLL linked by zig lld-link with the C runtime of spec (crt_spec), as zig cc -shared would link it."""
    s = json.loads(spec.read_text())
    return [zig, "lld-link", *s["flags"], f"-OUT:{out}", *flags, *inputs, *(str(spec.parent / f) for f in s["files"])]


def crt_link(game: Path, cc: list[str], lib: Path, out: Path, obj: Path, inputs: list[str]) -> list[str] | None:
    """The game library's Windows link with the package's C runtime, or None without it (crt_spec)."""
    spec = crt_spec(game, cc)
    if not spec:
        return None
    pdb = lib.with_name(lib.name + ".pdb")  # the name zig cc gave it
    return lld_link(
        cc[0], spec, out, inputs, f"-PDB:{pdb}", f"-PDBALTPATH:{pdb.name}", f"-IMPLIB:{obj / 'mkwgame.lib'}"
    )


def default_glslc(game: Path) -> str:
    base = package_dir(game) / "glslc"
    exe = base / ("glslc.exe" if os.name == "nt" else "bin/glslc")
    return str(exe) if exe.is_file() else "glslc"


def tool_env(glslc: str) -> dict[str, str]:
    """The package's glslc finds its libraries beside it (conda-forge layout: bin/ and lib/)."""
    env = dict(os.environ)
    lib = Path(glslc).parent.parent / "lib"
    if os.name != "nt" and lib.is_dir():
        env["LD_LIBRARY_PATH"] = f"{lib}:{env['LD_LIBRARY_PATH']}" if env.get("LD_LIBRARY_PATH") else str(lib)
    return env


def own_children() -> None:
    """A cancel from the start screen ends this process: its tools (gen.py's workers, the compilers, glslc) must end with
    it. POSIX: our own process group, killed on SIGTERM. Windows: a job object that kills its processes when its last
    handle, ours, closes (SDL_KillProcess is TerminateProcess there)."""
    if os.name != "nt":
        import signal

        os.setpgrp()
        signal.signal(signal.SIGTERM, lambda *_: os.killpg(0, signal.SIGKILL))
        return
    import ctypes
    from ctypes import wintypes

    class Limits(ctypes.Structure):  # JOBOBJECT_EXTENDED_LIMIT_INFORMATION
        _fields_ = [("basic", ctypes.c_byte * 64), ("io", ctypes.c_byte * 48), ("process_memory", ctypes.c_size_t),
                    ("job_memory", ctypes.c_size_t), ("peak_process", ctypes.c_size_t), ("peak_job", ctypes.c_size_t)]  # fmt: skip

    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    # Started by the start screen (a GUI program) python.exe gets a console window of its own; hidden here, and every
    # tool below (gen.py's workers, zig, glslc) shares that console instead of opening one each.
    k32.GetConsoleWindow.restype = wintypes.HWND
    console = k32.GetConsoleWindow()
    if console:
        ctypes.WinDLL("user32").ShowWindow(console, 0)  # SW_HIDE
    k32.CreateJobObjectW.restype = wintypes.HANDLE
    k32.GetCurrentProcess.restype = wintypes.HANDLE
    job = k32.CreateJobObjectW(None, None)
    lim = Limits()
    ctypes.c_uint32.from_buffer(lim.basic, 16).value = 0x2000  # LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
    k32.SetInformationJobObject(wintypes.HANDLE(job), 9, ctypes.byref(lim), ctypes.sizeof(lim))
    k32.AssignProcessToJobObject(wintypes.HANDLE(job), wintypes.HANDLE(k32.GetCurrentProcess()))


def mem_available_gb() -> float:
    """Memory free for new work now (MemAvailable on Linux, ullAvailPhys on Windows); 8 when unknown."""
    if os.name == "nt":
        try:
            return build.windows_memory_gb()[1]
        except OSError:
            return 8.0
    try:
        with open("/proc/meminfo") as f:
            for line in f:
                if line.startswith("MemAvailable:"):
                    return int(line.split()[1]) / 2**20
    except OSError:
        pass
    return 8.0


def shader_jobs() -> int:
    """Every core, as far as memory goes: a worker and its glslc take ~0.25 GB (four took 1.0 GB, 06/11/2026) and up
    to 0.75 GB on the largest programs (translate.py). Half the cores, as before, left 8 of 12 idle: 40 min."""
    cores = min(os.cpu_count() or 1, 61) if os.name == "nt" else os.cpu_count() or 1  # Windows' pool limit (#222)
    return max(1, min(cores, int(mem_available_gb() / 0.75)))


def mem_gb() -> float:
    try:
        return build.mem_total_gb()
    except OSError:
        return 8.0


def run(cmd: list[str], env: dict[str, str] | None = None, progress_prefix: str = "") -> None:
    """A tool's run; its 'progress n total' lines (translate.py) pass through, the rest goes to stderr."""
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env)
    tail = []
    for line in p.stdout:
        if progress_prefix and line.startswith(progress_prefix):
            emit(line.strip())
        else:
            sys.stderr.write(line)
            tail = (tail + [line.strip()])[-5:]
    if p.wait():
        raise Failed(f"{Path(cmd[1] if len(cmd) > 1 else cmd[0]).name} failed: {' / '.join(tail)}")
