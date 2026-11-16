"""Set the game up on the player's machine from their own copy: check it, copy it, build the game code, translate the
shaders into the player's chosen installation folder.

Why: nothing of the game ships with the port. The start screen (runtime/ui/launcher/launcher.cpp) runs this with the Python of
the player's package and shows its progress; the same steps run from a source checkout with the system's tools. The
order follows totk-pc-recomp's installer (docs/installer.md, MIT): check before writing, one key per step so a
cancelled or failed run resumes from the last finished step.

Steps, into --game (by default the folder of mkw / mkw.exe):
  check     exefs/main is a version of MKW the port knows (NSO build id, recomp/versions.py: 1.0.0, 1.8.0), the RomFS
            has at least 95 % of that version's files (1.0.0: 205 660)
  exefs     exefs/ copied (40 MB)
  romfs     romfs/ copied (23 GB), or --romfs link: its path written to mkw.ini (romfs = ...)
  generate  recomp/gen.py: the game code as C, in <game>/setup-work
  compile   that C with the package's zig (or clang) into mkwgame.so / mkwgame.dll
  shaders   the SM86 spec built here from NVIDIA's nvdisasm (sm86/spec/spec.py, into sm86/; never shipped), then
            tools/shaders/extract.py and translate.py into spirv/
  warm      the game run once for a minute: the driver's pipeline cache and the pipeline keys for the first start
  cleanup   setup-work removed but its obj/ and zig-cache/: the next update compiles only the chunks whose C changed
            (gamecode.py), and zig does not build its libc again

The copy is loose exefs/ and romfs/ folders, or a .nup package (the Switch 2 dump format, nup.py): --source names the
package file itself. From a package, exefs/ is extracted, romfs/ extracted (copy) or the package itself
linked (the runtime reads its RomFS in place).

Output: one event per line on stdout, for the start screen: "step <name>", "progress <done> <total>", "skip <name>",
"done", "error <code> <detail>". Exit codes: 0 done, 1 failed, 2 bad command line. The start screen shows its own
translated text per code with the detail inside: source (no earlier setup to repeat), moved (the last setup's folder,
gone: moved or renamed, #188), nogame (the folder, without exefs/main and romfs/), encrypted (exefs/main, not an NSO), version (main's build id), romfs (its file count), failed
(a tool's English message) and io (the system's); logs/setup.log gets the English message whole.

Usage: python prepare.py --source DIR|FILE.nup --game DIR [--runtime DIR] [--romfs copy|link] [--jobs N]
                         [--target linux|windows]
Without --source (the launcher after an update, tools/prepare/update.py): the copy and the RomFS mode of the last
setup in --game, from its setup-state.json; only the steps whose key changed run again.
"""

import faulthandler
import sys
from pathlib import Path


def _early_log() -> None:
    """logs/setup.log opened before anything else runs, and every two minutes the Python stack of each thread written
    to it: a setup that sat at 0 % CPU before writing a line left no log at all, so nothing said where it waited
    (#251, Windows, 0.1.10). Rewritten at every setup; main() keeps writing to the same file."""
    try:
        game = Path(sys.argv[sys.argv.index("--game") + 1]).absolute()  # as main()'s, so it keeps this file
        (game / "logs").mkdir(parents=True, exist_ok=True)
        sys.stderr = open(game / "logs/setup.log", "w", encoding="utf-8", buffering=1)  # noqa: SIM115
    except ValueError, IndexError, OSError:
        return
    print(f"setup: started, Python {sys.version.split()[0]}, {sys.executable}", file=sys.stderr)
    faulthandler.dump_traceback_later(120, repeat=True, file=sys.stderr)


_early_log()

import argparse  # noqa: E402
import hashlib  # noqa: E402
import json
import os
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]  # the repository, or the package's setup/src
sys.path.insert(0, str(ROOT / "tools/build"))
sys.path.insert(0, str(ROOT / "tools/i18n"))
import build  # (GEN_FLAGS, module_glue, game_id: the same game code as build.py --dist)
import game_texts  # (build_all: the game's texts in ca, gl, eu)

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gamecode import DamagedSource, compile_game
from gamesource import check, is_nx, loose_romfs, unpack
from gamecopy import clear_dir, copy_tree, remove_stale
from nup import Nup
from machine import (
    ZIG_CACHE,
    Failed,
    default_cc,
    default_glslc,
    emit,
    mem_available_gb,
    mem_gb,
    own_children,
    run,
    shader_jobs,
)
from shaderstep import shaders, spec_command, translator_key  # noqa: F401  (spec_command: tests/test_setup.py)
import update  # (ca_file)

PREP_VERSION = 1  # bump when a step's output changes meaning: every step runs again
# (build.py put recomp/ on the path)
from versions import Version

SHADER_DIRS = ("Effect", "Layout", "Lib")  # extract.py's SHADER_DIRS: with Shader/, the RomFS folders it reads
STEPS = ("check", "exefs", "romfs", "generate", "compile", "shaders", "warm", "cleanup")
WARM_SECONDS = 60  # the driver cache is saved every 20 s and the pipeline keys every 2 (vk_pipeline.c, vk_pipe_keys.c)


def ini_set(ini: Path, key: str, value: str) -> None:
    """key = value in mkw.ini (runtime/core/base/cfg.c format), replacing a line with that key."""
    lines = ini.read_text(encoding="utf-8").splitlines() if ini.is_file() else []
    norm = key.upper().removeprefix("MKW_")
    keep = [ln for ln in lines if ln.split("=", 1)[0].strip().upper().removeprefix("MKW_") != norm]
    ini.write_text("\n".join([*keep, f"{key} = {value}"]) + "\n", encoding="utf-8")


def ini_get(ini: Path, key: str) -> str:
    """key's value in mkw.ini, "" when it has none."""
    norm = key.upper().removeprefix("MKW_")
    for ln in ini.read_text(encoding="utf-8").splitlines() if ini.is_file() else []:
        k, sep, v = ln.partition("=")
        if sep and k.strip().upper().removeprefix("MKW_") == norm:
            return v.strip()
    return ""


def code_mods(game: Path) -> tuple[list[dict], str]:
    """The enabled mods' code bytes (mod.json "image", tools/mod/ips.py), each with its mod's name, for gen.py --image;
    and a key over them and every installed mod's switchable "patches" (gen.py compiles those in, recomp/patches.py
    load_mods): "" without any, so a setup without code mods keeps its keys."""
    enabled = [m.strip() for m in ini_get(game / "mkw.ini", "mods").split(",") if m.strip()]
    image, switchable = [], {}
    mods = game / "mods"
    for mj in sorted(mods.glob("*/mod.json")) if mods.is_dir() else []:
        try:
            meta = json.loads(mj.read_text(encoding="utf-8"))
        except OSError, ValueError:
            continue
        if meta.get("patches"):
            switchable[mj.parent.name] = meta["patches"]
        if mj.parent.name in enabled:
            image += [{"mod": mj.parent.name, **p} for p in meta.get("image") or []]
    if not image and not switchable:
        return [], ""
    blob = json.dumps([image, switchable], sort_keys=True).encode()
    return image, hashlib.sha256(blob).hexdigest()[:16]



def generate(exefs: Path, work: Path, jobs: int, v: Version, image: list[dict]) -> None:
    gen = work / "generated"
    cmd = [sys.executable, str(ROOT / "recomp/gen.py"), "--exefs", str(exefs), "--out", str(gen), "--jobs", str(jobs),
           "--patches", str(v.patches), "--mods", str(exefs.parent / "mods")]  # fmt: skip
    if image:  # the enabled code mods' bytes, lifted in place
        (work / "image.json").write_text(json.dumps(image), encoding="utf-8")
        cmd += ["--image", str(work / "image.json")]
    run(cmd)


def code_key(files: list[Path]) -> str:
    """Generate's and compile's keys besides the ExeFS and game_id: the code that makes their output. Before, they were
    game_id alone, so an update that changed only recomp/ (or the glue and flags) kept the old game code."""
    h = hashlib.sha256()
    for f in files:
        h.update(f.read_bytes())
    return h.hexdigest()[:16]


def migrate_keys(state: dict, keys: dict[str, str], before: dict[str, str]) -> None:
    """A step done under its key from before 0.1.10 (with the source folders, `before`) is taken as done under the new
    one: else the first update after the change would set up everything again."""
    for step, old in before.items():
        if state["done"].get(step) == old:
            state["done"][step] = keys[step]


def regenerate_if_cleaned(state: dict, keys: dict[str, str], work: Path) -> None:
    """Undoes generate when compile must run and cleanup already removed generated/: a compile whose key changed alone
    (an update that touched only tools/build/build.py, as the first after 0.1.9) stopped on the missing layout.json."""
    if state["done"].get("compile") != keys["compile"] and not (work / "generated/layout.json").is_file():
        state["done"].pop("generate", None)


def redo_exefs_if_missing(state: dict, game: Path) -> None:
    """Undoes the exefs step when its copy is gone: the step is skipped by its key (source and version), and a folder
    emptied or moved since made generate stop on "module main not in the layout" (#419), which names no cause."""
    if "exefs" in state["done"] and not (game / "exefs/main").is_file():
        state["done"].pop("exefs")


GENERATOR = [
    *sorted((ROOT / "recomp").glob("*.py")),
    *sorted((ROOT / "recomp").glob("patches-mkw*.txt")),
]  # each version's table
COMPILER = [*GENERATOR, ROOT / "tools/build/build.py", Path(__file__).with_name("gamecode.py")]


def warm(game: Path, work: Path, runtime: Path | None = None) -> None:
    """The game run once to its title, so the first real start finds pipeline-cache.bin and pipeline-keys.bin. Its
    saves go to a scratch profile: the player's first start stays a first start. The window opens without taking focus
    and without sound; the run is ended after WARM_SECONDS. Its log is logs/warm.log: in mkw.log it pushed the player's
    last run out of the file reports attach and the start screen reads (#437, 16/11/2026)."""
    exe = (runtime or game) / ("mkw.exe" if os.name == "nt" else "mkw")
    env = dict(os.environ, MKW_LAUNCHED="1", MKW_ASSUME_FOCUS="1", MKW_AUDIO="none", MKW_LOG_NAME="warm",
               MKW_GAME_DIR=str(game), MKW_PROFILE_DIR=str(work / "warm-profile"))  # fmt: skip
    p = subprocess.Popen([str(exe)], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for t in range(WARM_SECONDS):
        if p.poll() is not None:
            raise Failed(f"the game stopped while warming up (exit {p.returncode}); its log is {game / 'logs/warm.log'}")
        emit("progress", t, WARM_SECONDS)
        time.sleep(1)
    p.terminate()
    try:
        p.wait(10)
    except subprocess.TimeoutExpired:
        p.kill()
    emit("progress", WARM_SECONDS, WARM_SECONDS)


def load_state(game: Path) -> dict:
    p = game / "setup-state.json"
    try:
        s = json.loads(p.read_text())
    except OSError, ValueError:
        return {"done": {}}
    return s if s.get("version") == PREP_VERSION else {"done": {}}


def save_state(game: Path, state: dict) -> None:
    state["version"] = PREP_VERSION
    p = game / "setup-state.json"
    p.with_suffix(".tmp").write_text(json.dumps(state, indent=1))
    p.with_suffix(".tmp").replace(p)


def main() -> None:
    # This Python runs with -I, but the tools it starts (gen.py, spec.py, translate.py and their workers) would take the
    # player's PYTHONPATH or PYTHONHOME and load another Python's modules: "Module use of python312.dll conflicts" (#177)
    for k in [k for k in os.environ if k.startswith("PYTHON")]:
        del os.environ[k]
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument(
        "--source",
        type=Path,
        help="the folder with the player's exefs/ and romfs/, or their .nup package (default: the last's)",
    )
    ap.add_argument("--game", type=Path, required=True, help="the game installation folder")
    ap.add_argument("--runtime", type=Path, help="the folder of mkw / mkw.exe (default: --game)")
    ap.add_argument("--romfs", choices=("copy", "link"), help="(default: the last setup's, else copy)")
    ap.add_argument("--jobs", type=int, default=0, help="parallel jobs (default: from cores and memory)")
    ap.add_argument("--cc", help="the C compiler command (default: the package's zig cc, else clang)")
    ap.add_argument("--glslc", help="(default: the package's, else glslc on PATH)")
    ap.add_argument("--target", choices=("linux", "windows"), default="windows" if os.name == "nt" else "linux")
    ap.add_argument(
        "--port", default="", help="the port version this setup is for (the start screen's): kept in the state"
    )
    args = ap.parse_args()
    own_children()
    if ca := update.ca_file():  # spec.py downloads NVIDIA's redistributables with this Python (issue #102, NixOS)
        os.environ["SSL_CERT_FILE"] = ca
    # Keep the launcher's accepted spelling: resolving a short symlink/junction can expand beyond the runtime's
    # directory limit, making warm-up reject the very installation this setup just built.
    game = args.game.absolute()
    runtime = args.runtime.resolve() if args.runtime else game
    state = load_state(game)
    if not args.source:  # the last setup's; before 0.1.5 the state kept them only in its keys
        done_exefs = Path(state["done"]["exefs"]) if "exefs" in state["done"] else None
        last = state.get("source") or (
            str(done_exefs if done_exefs.is_file() else done_exefs.parent) if done_exefs else ""
        )
        if not last:
            emit("error", "source", "no earlier setup in this folder")
            sys.exit(2)
        args.source = Path(last)
        if not args.source.exists():
            emit("error", "moved", last)
            sys.exit(1)
        args.romfs = args.romfs or state.get("romfs_mode") or state["done"].get("romfs", "copy").split(" ", 1)[0]
    args.romfs = args.romfs or "copy"
    src = args.source.resolve()
    state["source"], state["romfs_mode"] = str(src), args.romfs
    # The start screen reads only stdout: the tools' output and the timings go to logs/setup.log, the file a report
    # attaches (issue #2 had none to give).
    (game / "logs").mkdir(parents=True, exist_ok=True)
    if getattr(sys.stderr, "name", "") != str(game / "logs/setup.log"):  # _early_log opened it unless --game was odd
        sys.stderr = open(game / "logs/setup.log", "w", encoding="utf-8", buffering=1)  # noqa: SIM115
    print(f"setup: {' '.join(sys.argv)}", file=sys.stderr)

    # Within 60 % of the RAM, by what one job was measured to take (13/11/2026, 12 at once): a compile 0.5 GB (peaks
    # 266-409 MB, build.py --job-mem-gb), a generate worker 0.7 GB (gen.py: 5 065 MB for 12, one up to 608 MB). 1 GB for
    # both kept a 12-thread PC with 13 GB at 8 compiles and an 8 GB one at 4.
    # Windows, 15/11/2026 (24 threads, 31 GB): 24 generate workers held 26.6 GB of private memory, 1.1 GB each (cause
    # not measured), and took the free memory from 8.9 GB to 0.13 GB; 24 compiles held 7.6 GB, 0.32 GB each. So a
    # Windows worker counts 1.2 GB (the 1.1 measured plus a margin, as 0.7 is for 0.42 on Linux). The budget is also
    # what is free when the setup starts (the player's browser and launcher stay open), as shader_jobs does on every
    # system; on Linux MemAvailable counts the reclaimable cache, and it lowers the jobs only when less than 60 % of the
    # RAM is available (not measured on Linux).
    free = mem_available_gb()
    budget = min(mem_gb() * 0.6, free)

    def fit(gb: float) -> int:
        return args.jobs or max(1, min(os.cpu_count() or 1, int(budget / gb)))

    jobs, gen_jobs = fit(0.5), fit(1.2 if os.name == "nt" else 0.7)
    # setup.log only (stderr): what the machine gave the setup, and below each step's time, so two setups (Linux and
    # Windows, or a player's and ours) compare step by step (Adrià 13/11/2026: "los logs de preparación deberían
    # guardar tiempos"; 0.1.10's Windows setup took twice the Linux one and nothing said where)
    print(
        f"machine: {os.cpu_count()} threads, {mem_gb():.1f} GB, {free:.1f} GB free, {jobs} compile and {gen_jobs} "
        f"generate jobs, {sys.platform}",
        file=sys.stderr,
    )
    t_all, step, t_step = time.monotonic(), "check", time.monotonic()
    glslc = args.glslc or default_glslc(game)
    work = game / "setup-work"
    try:
        emit("step", "check")
        cc = args.cc.split() if args.cc else default_cc(game, args.target)
        exefs_src, romfs_src, version = check(src, game, runtime)
        romfs = game / "romfs" if args.romfs == "copy" else romfs_src
        # The version (from exefs/main's build id) says which game a step built from; the folder only matters to the
        # steps that copy from it or point mkw.ini at it. With the folder in every key, moving or renaming it redid
        # generate, compile and the shaders (#188: 16 min on Linux, 31 on Windows)
        # code mods: their key joins generate's and compile's only when there are some (a mod enabled, disabled,
        # imported or deleted builds the game code again; tools/mod/ips.py)
        image, mods_key = code_mods(game)
        with_mods = f" mods {mods_key}" if mods_key else ""
        keys = {
            "exefs": str(exefs_src),
            "romfs": f"copy {version.name}" if args.romfs == "copy" else f"link {romfs_src}",
            "generate": f"{build.game_id()} {version.name} {code_key(GENERATOR)}{with_mods}",
            "compile": f"{build.game_id()} {version.name} {args.target} {code_key(COMPILER)}{with_mods}",
            "shaders": f"{version.name} {translator_key()}",
            "warm": f"{build.game_id()} {version.name}",
        }
        migrate_keys(state, keys, {
            "romfs": f"{args.romfs} {romfs_src}",
            "generate": f"{build.game_id()} {exefs_src} {version.name} {code_key(GENERATOR)}",
            "compile": f"{build.game_id()} {exefs_src} {version.name} {args.target} {code_key(COMPILER)}",
            "shaders": f"{exefs_src} {romfs_src} {translator_key()}",
            "warm": f"{build.game_id()} {romfs_src}",
        })  # fmt: skip
        regenerate_if_cleaned(state, keys, work)
        redo_exefs_if_missing(state, game)
        for step in STEPS[1:]:
            # The start screen sets up again when its version differs (launcher.cpp): an update by hand (a new
            # AppImage or zip over the old) kept the old steps (#50: 0.1.8 played with 0.1.7's shaders). Kept once the
            # game is built, before the optional warm-up: a warm-up that keeps failing (#50's hang) must not repeat
            # the setup at every start.
            if step == "warm" and args.port:
                state["port"] = args.port
                save_state(game, state)
            key = keys.get(step, "")
            if step != "cleanup" and state["done"].get(step) == key:
                emit("skip", step)
                continue
            emit("step", step)
            t_step = time.monotonic()
            if step == "exefs":
                if is_nx(exefs_src):
                    clear_dir(game / "exefs", exefs_src)
                    unpack(runtime, exefs_src, game, "exefs")
                elif exefs_src.is_file():
                    nup = Nup(exefs_src)
                    nup.extract("exefs", game / "exefs", lambda d, t: emit("progress", d, t))
                    remove_stale(game / "exefs", exefs_src, set(nup.files("exefs")), version.name)
                else:
                    copy_tree(exefs_src, game / "exefs", sum(1 for p in exefs_src.iterdir() if p.is_file()),
                              version.name)
            elif step == "romfs":
                if args.romfs == "copy" and is_nx(romfs_src):
                    clear_dir(romfs, romfs_src)
                    unpack(runtime, romfs_src, game, "romfs")
                    ini_set(game / "mkw.ini", "romfs", "romfs")
                elif args.romfs == "copy" and romfs_src.is_file():
                    nup = Nup(romfs_src)
                    nup.extract("romfs", romfs, lambda d, t: emit("progress", d, t))
                    remove_stale(romfs, romfs_src, set(nup.files("romfs")), version.name)
                    ini_set(game / "mkw.ini", "romfs", "romfs")
                elif args.romfs == "copy":
                    copy_tree(romfs_src, romfs, version.romfs_files, version.name)
                    ini_set(game / "mkw.ini", "romfs", "romfs")
                else:
                    ini_set(game / "mkw.ini", "romfs", str(romfs_src))
            elif step == "generate":
                generate(game / "exefs", work, gen_jobs, version, image)
            elif step == "compile":
                try:
                    compile_game(game, game / "exefs", work, cc, args.target, jobs)
                except DamagedSource as e:  # once: a second damaged write is the disk or the memory, not chance
                    print(f"{e}: generating again", file=sys.stderr)
                    emit("step", "generate")
                    generate(game / "exefs", work, gen_jobs, version, image)
                    emit("step", "compile")
                    compile_game(game, game / "exefs", work, cc, args.target, jobs)
            elif step == "shaders":
                # extract.py reads a .nup itself; a .nx's shader archives are unpacked first
                src_romfs = loose_romfs(runtime, romfs, work, "Shader", *SHADER_DIRS) if is_nx(romfs) else romfs
                shaders(game / "exefs", src_romfs, game, work, glslc, args.jobs or shader_jobs(), cc)
            elif step == "warm":
                warm(game, work, runtime)
            elif step == "cleanup":
                for p in work.glob("*"):
                    if p.is_dir() and p.name not in ("obj", ZIG_CACHE):
                        shutil.rmtree(p, ignore_errors=True)
                # the languages the game does not ship (ca, gl, eu), from this version's translations (the step runs
                # at every setup, so an update brings its new texts); about a second
                game_texts.build_all(game, loose_romfs(runtime, romfs, work, "Mals"))
                shutil.rmtree(work / "pack", ignore_errors=True)
            state["done"][step] = key
            save_state(game, state)
            print(f"time: {step} {time.monotonic() - t_step:.1f} s", file=sys.stderr)
        print(f"time: setup {time.monotonic() - t_all:.1f} s", file=sys.stderr)
        emit("done")
    except Failed as e:
        print(f"time: {step} failed after {time.monotonic() - t_step:.1f} s", file=sys.stderr)
        print(f"failed: {e}", file=sys.stderr)
        emit("error", e.code, e.detail)
        sys.exit(1)
    except OSError as e:
        emit("error", "io", e)
        sys.exit(1)


if __name__ == "__main__":
    main()
