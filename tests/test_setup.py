"""The setup's command lines and files without the game: paths with spaces and accents, the resume without --source and
the update's checks. Seconds, on Linux and on Windows (ci.yml), so a break like issue #29 (a compiler path split at its
space, found by a player) shows in the commit that makes it.

Usage: uv run pytest tests
"""

import json
import os
import shutil
import subprocess
import sys
from pathlib import Path
from types import SimpleNamespace

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "sm86/spec"))
sys.path.insert(0, str(ROOT / "tools/prepare"))
import prepare  # noqa: E402
import gamesource  # noqa: E402  (after prepare: it puts recomp/ on the path)
import gamecode  # noqa: E402
import machine  # noqa: E402
import spec  # noqa: E402
import update  # noqa: E402

ODD = "Recomp C ñ"  # a space and an accent, as the players' folders have


def test_cc_survives_spaces(tmp_path: Path) -> None:
    # the shaders step's command line as prepare.py builds it, read back as spec.py's argparse does
    cc = [str(tmp_path / ODD / "zig.exe"), "cc", "-target", "x86_64-windows-gnu"]
    cmd = prepare.spec_command(tmp_path / ODD, tmp_path / ODD / "work", cc, None)
    assert spec.cc_command(cmd[cmd.index("--cc") + 1]) == cc
    assert spec.cc_command("zig cc") == ["zig", "cc"]  # by hand


def run_prepare(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, "-I", str(ROOT / "tools/prepare/prepare.py"), *args],
                          capture_output=True, text=True, timeout=60)  # fmt: skip


@pytest.mark.parametrize("external", [False, True])
def test_resume_without_source(tmp_path: Path, external: bool) -> None:
    game = tmp_path / ODD / "game"
    game.mkdir(parents=True)
    copy = tmp_path / ODD / "copy"
    copy.mkdir()
    state = {"version": 1, "done": {"exefs": str(copy / "exefs"), "romfs": f"link {copy / 'romfs'}"}}
    (game / "setup-state.json").write_text(json.dumps(state))
    p = run_prepare("--game", str(game), *(["--runtime", str(tmp_path / "Launcher ñ")] if external else []))
    # the last setup's copy, whole: check fails on its missing files, naming the folder with its space
    assert "step check" in p.stdout, p.stdout + p.stderr
    assert str(copy) in p.stdout, p.stdout
    saved = json.loads((game / "setup-state.json").read_text())
    assert saved.get("source", str(copy)) == str(copy)


def test_resume_from_moved_folder(tmp_path: Path) -> None:
    # issue #188: the last setup's folder renamed; the start screen names it and asks for it again
    game = tmp_path / ODD / "game"
    game.mkdir(parents=True)
    gone = tmp_path / ODD / "renamed away"
    (game / "setup-state.json").write_text(json.dumps({"version": 1, "source": str(gone), "done": {}}))
    p = run_prepare("--game", str(game))
    assert p.returncode == 1 and f"error moved {gone}" in p.stdout, p.stdout + p.stderr


def test_keys_from_before_kept() -> None:
    # a step done under its key with the source folders (before 0.1.10) is not redone; another key still is
    state = {"done": {"shaders": "/old/exefs /old/romfs t1", "compile": "/old/exefs c1"}}
    prepare.migrate_keys(state, {"shaders": "1.0.0 t1", "compile": "1.0.0 c2"},
                         {"shaders": "/old/exefs /old/romfs t1", "compile": "/old/exefs c2"})  # fmt: skip
    assert state["done"] == {"shaders": "1.0.0 t1", "compile": "/old/exefs c1"}


def test_resume_without_earlier_setup(tmp_path: Path) -> None:
    p = run_prepare("--game", str(tmp_path / ODD))
    assert p.returncode == 2 and "error source" in p.stdout, p.stdout


@pytest.mark.skipif(os.name == "nt", reason="creating directory symlinks requires Windows privileges")
def test_early_setup_log_preserves_alias(tmp_path: Path) -> None:
    target, alias, source = tmp_path / "real", tmp_path / "Game link ñ", tmp_path / "empty dump"
    target.mkdir()
    source.mkdir()
    alias.symlink_to(target, target_is_directory=True)
    p = run_prepare("--game", str(alias), "--source", str(source))
    assert p.returncode == 1 and "error nogame" in p.stdout, p.stdout + p.stderr
    log = (alias / "logs/setup.log").read_text(encoding="utf-8")
    # main() must keep _early_log's file open under the same alias, retaining startup diagnostics and its timer.
    assert "setup: started, Python" in log
    assert "setup: " in log and str(alias) in log


def test_versions() -> None:
    assert update.version_key("0.1.2-12-g1c15097") == (0, 1, 2)
    assert update.version_key("v0.1.10") > update.version_key("0.1.9")
    assert update.version_key("dev") is None


def test_romfs_version_matches_main() -> None:
    # #315: 1.0.0's exefs/main with 1.8.0's RomFS passed the file count and crashed while warming up
    import versions

    v100, v180 = versions.VERSIONS
    res = "System/Resource/ResourceSizeTable."
    r100 = [f"{res}Product.100.Nin_Ounce_NVN2.rsizetable.zs", f"{res}Pilot.100.Nin_Ounce_NVN2.rsizetable.zs"]
    r180 = [f"{res}Product.180.Nin_Ounce_NVN2.rsizetable.zs", "Layout/Title.sarc.zs"]
    assert versions.romfs_mismatch(v100, r100) == "" and versions.romfs_mismatch(v180, r180) == ""
    assert versions.romfs_mismatch(v100, r180) == "RomFS 1.8.0, exefs/main 1.0.0"
    assert versions.romfs_mismatch(v180, r100) == "RomFS 1.0.0, exefs/main 1.8.0"
    assert versions.romfs_mismatch(v100, ["Layout/Title.sarc.zs"]) == ""  # no marker: the file count decides


@pytest.mark.parametrize("product, ok", [(100, True), (180, False)])
def test_check_refuses_other_romfs(tmp_path: Path, monkeypatch, product: int, ok: bool) -> None:
    # check() on a loose dump: 1.0.0's main (its build id in an NSO header) and a RomFS naming `product`
    import versions

    v100 = versions.VERSIONS[0]
    (tmp_path / "exefs").mkdir()
    (tmp_path / "exefs/main").write_bytes(b"NSO0" + bytes(0x3C) + bytes.fromhex(v100.main_build_id))
    res = tmp_path / "romfs/System/Resource"
    res.mkdir(parents=True)
    (res / f"ResourceSizeTable.Product.{product}.Nin_Ounce_NVN2.rsizetable.zs").write_bytes(b"")
    monkeypatch.setattr(gamesource, "count_files", lambda top, enough: enough)  # the file count is not under test
    if ok:
        assert prepare.check(tmp_path, tmp_path)[2] == v100
    else:
        with pytest.raises(prepare.Failed) as e:
            prepare.check(tmp_path, tmp_path)
        assert "RomFS 1.8.0, exefs/main 1.0.0" in str(e.value)


@pytest.mark.parametrize("name", ["stale.old", "setup.old"])
def test_clean_removes_old(tmp_path: Path, name: str) -> None:
    game = tmp_path / ODD
    (game / "update").mkdir(parents=True)
    old = game / name
    old.mkdir() if name == "setup.old" else old.write_text("x")
    (game / "mkw.exe").write_text("keep")
    update.clean(type("A", (), {"game": game, "previous": None})())
    assert not old.exists() and not (game / "update").exists() and (game / "mkw.exe").exists()


def test_clean_keeps_previous(tmp_path: Path) -> None:
    # with --previous the replaced files go to update-previous/ under their own names, for the Tools page's Undo
    game = tmp_path / ODD
    (game / "update-previous").mkdir(parents=True)
    (game / "update-previous/gone.dll").write_text("older still")
    (game / "mkw.exe.old").write_text("0.1.10")
    (game / "lang.old").mkdir()
    update.clean(type("A", (), {"game": game, "previous": "0.1.10"})())
    keep = game / "update-previous"
    assert (keep / "mkw.exe").read_text() == "0.1.10" and (keep / "lang").is_dir()
    assert (keep / "version").read_text().strip() == "0.1.10" and not (keep / "gone.dll").exists()
    assert not list(game.glob("*.old"))


def test_package_sources_import(tmp_path: Path) -> None:
    # setup/src as dist.py copies it: prepare.py and what it imports load without the rest of the repository (build.py
    # imported tools/i18n/catalog.py, which the package lacked)
    sys.path.insert(0, str(ROOT / "tools/build"))
    import dist  # noqa: PLC0415

    dist.copy_sources(tmp_path / "src")
    p = subprocess.run([sys.executable, "-I", str(tmp_path / "src/tools/prepare/prepare.py"), "--help"],
                       capture_output=True, text=True, timeout=60)  # fmt: skip
    assert p.returncode == 0, p.stderr


def test_package_tools_stay_beside_launcher(tmp_path: Path) -> None:
    sys.path.insert(0, str(ROOT / "tools/build"))
    import dist  # noqa: PLC0415

    setup = tmp_path / ODD / "launcher/setup"
    dist.copy_sources(setup / "src")
    code = """
import sys
from pathlib import Path
sys.path.insert(0, sys.argv[1])
import machine
assert machine.package_dir(Path(sys.argv[2])) == Path(sys.argv[3])
"""
    p = subprocess.run([sys.executable, "-I", "-c", code, str(setup / "src/tools/prepare"),
                        str(tmp_path / "Other installation ñ"), str(setup)],
                       capture_output=True, text=True, timeout=60)  # fmt: skip
    assert p.returncode == 0, p.stderr


def test_warm_uses_launcher_with_selected_installation(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    game, runtime = tmp_path / ODD / "game", tmp_path / "launcher"
    calls = []
    process = SimpleNamespace(terminate=lambda: None, wait=lambda timeout: 0)

    def start(argv: list[str], **kwargs: object) -> SimpleNamespace:
        calls.append((argv, kwargs))
        return process

    monkeypatch.setattr(prepare.subprocess, "Popen", start)
    monkeypatch.setattr(prepare, "WARM_SECONDS", 0)
    prepare.warm(game, game / "setup-work", runtime)
    argv, kwargs = calls[0]
    assert argv == [str(runtime / ("mkw.exe" if os.name == "nt" else "mkw"))]
    assert kwargs["env"]["MKW_GAME_DIR"] == str(game)
    assert kwargs["env"]["MKW_PROFILE_DIR"] == str(game / "setup-work/warm-profile")


@pytest.mark.skipif(os.name == "nt", reason="creating directory symlinks requires Windows privileges")
def test_setup_preserves_short_alias_for_warmup(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    target = tmp_path / "real"
    for _ in range(6):
        target /= "deep" * 40
    target.mkdir(parents=True)
    assert len(str(target).encode()) > 768
    alias = tmp_path / "Game link ñ"
    alias.symlink_to(target, target_is_directory=True)
    runtime, source = tmp_path / "launcher", tmp_path / "dump"
    calls = []
    process = SimpleNamespace(terminate=lambda: None, wait=lambda timeout: 0)

    def start(argv: list[str], **kwargs: object) -> SimpleNamespace:
        calls.append((argv, kwargs))
        return process

    monkeypatch.setattr(prepare, "own_children", lambda: None)
    monkeypatch.setattr(prepare.update, "ca_file", lambda: None)
    monkeypatch.setattr(prepare, "STEPS", ("check", "warm"))
    monkeypatch.setattr(prepare, "check", lambda *args: (source / "exefs", source / "romfs",
                                                       SimpleNamespace(name="test")))
    monkeypatch.setattr(prepare, "WARM_SECONDS", 0)
    monkeypatch.setattr(prepare.subprocess, "Popen", start)
    monkeypatch.setattr(sys, "argv", ["prepare.py", "--game", str(alias), "--runtime", str(runtime),
                                      "--source", str(source)])
    # main opens its setup log on stderr. Restore pytest's capture stream after exercising the real argument path
    # and warm-up environment; only the game check and game process are synthetic.
    original_stderr = sys.stderr
    monkeypatch.setattr(sys, "stderr", original_stderr)
    try:
        prepare.main()
    finally:
        if sys.stderr is not original_stderr:
            sys.stderr.close()
    argv, kwargs = calls[0]
    assert argv == [str(runtime / "mkw")]
    assert kwargs["env"]["MKW_GAME_DIR"] == str(alias)
    assert kwargs["env"]["MKW_PROFILE_DIR"] == str(alias / "setup-work/warm-profile")


@pytest.mark.parametrize("driver", ["clang", "lld-link", "zig"])
@pytest.mark.parametrize("name", ["Space and ñ", 'Quote" ñ', "Backslash\\ ñ", 'Both" \\ ñ '])
def test_link_response_preserves_paths(tmp_path: Path, driver: str, name: str) -> None:
    windows = driver == "lld-link"
    if os.name == "nt" and (not windows or any(c in name for c in '\\"') or name.endswith(" ")):
        pytest.skip("POSIX linker or directory name")
    cc = shutil.which("clang")
    linker = shutil.which(driver)
    if not linker and driver == "zig":
        packaged = ROOT / "build/dist/linux/setup/zig/zig"
        linker = str(packaged) if packaged.is_file() else None
    if not cc or not linker:
        pytest.skip("clang and the requested linker are needed (zig may come from the local release package)")
    folder = tmp_path / name
    folder.mkdir()
    src, obj, rsp = folder / "stub.c", folder / "stub.o", folder / "link.rsp"
    src.write_text("int response_file_probe(void) { return 7; }\n")
    flags = ["--target=x86_64-windows-gnu"] if windows else ["-fPIC"]
    subprocess.run([cc, *flags, "-c", str(src), "-o", str(obj)], check=True)
    gamecode.write_link_response(rsp, [obj], windows or driver == "zig")
    lib = folder / ("stub.dll" if windows else "stub.so")
    args = ([linker, "/dll", "/noentry", "/export:response_file_probe", f"/out:{lib}", f"@{rsp}"] if windows
            else [linker, *(["cc"] if driver == "zig" else []), "-shared", "-o", str(lib), f"@{rsp}"])
    # Reuse Zig's compiler runtime across cases; the response file and object paths remain different each time.
    env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(tmp_path.parent / "setup-link-zig-cache"),
               ZIG_LOCAL_CACHE_DIR=str(tmp_path / "zig-local-cache"))
    p = subprocess.run(args, env=env, capture_output=True, text=True, timeout=120)
    assert p.returncode == 0, p.stderr
    assert lib.is_file()


def test_unpack_uses_launcher_not_installation(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    runtime, game = tmp_path / ODD / "launcher", tmp_path / "installation"
    calls = []
    process = SimpleNamespace(stdout=["unpack: 1 files, 64 bytes\n"], wait=lambda: 0)

    def start(argv: list[str], **kwargs: object) -> SimpleNamespace:
        calls.append(argv)
        return process

    monkeypatch.setattr(gamesource.subprocess, "Popen", start)
    package = tmp_path / "Dump ñ.nx"
    assert gamesource.unpack(runtime, package, game, "exefs") == "unpack: 1 files, 64 bytes"
    assert calls == [[str(runtime / ("mkw.exe" if os.name == "nt" else "mkw")),
                      "--unpack", str(package), str(game), "exefs"]]


def test_find_game_packages(tmp_path: Path) -> None:
    # a .nup or .nx package is the source as one file (its exefs and romfs both); any other file is refused
    for name, magic in (("g.nup", b"NUP\x01"), ("g.nx", b"NXAR")):
        (tmp_path / name).write_bytes(magic + bytes(60))
        assert gamesource.find_game(tmp_path / name) == (tmp_path / name, tmp_path / name)
    (tmp_path / "g.zip").write_bytes(b"PK\x03\x04" + bytes(60))
    with pytest.raises(prepare.Failed):
        gamesource.find_game(tmp_path / "g.zip")


def test_regenerate_if_cleaned(tmp_path: Path) -> None:
    # an update that changes only the compile key, after cleanup removed generated/: generate runs again
    keys = {"generate": "g", "compile": "c2"}
    state = {"done": {"generate": "g", "compile": "c1"}}
    prepare.regenerate_if_cleaned(state, keys, tmp_path)
    assert "generate" not in state["done"]
    # generated/ still there, or compile already done: generate stays skipped
    (tmp_path / "generated").mkdir()
    (tmp_path / "generated/layout.json").write_text("[]")
    state = {"done": {"generate": "g", "compile": "c1"}}
    prepare.regenerate_if_cleaned(state, keys, tmp_path)
    assert state["done"]["generate"] == "g"
    state = {"done": {"generate": "g", "compile": "c2"}}
    prepare.regenerate_if_cleaned(state, keys, tmp_path / "none")
    assert state["done"]["generate"] == "g"


def test_fetch_retries_damaged_downloads(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    # issue #207: each try got another checksum; a damaged download is fetched again, and after the last a clear error
    import hashlib

    good = b"nvidia package"
    arrivals = [b"nvidia pa", b"nvidia packagX", good]

    def download(url: str, part: Path) -> str:
        part.write_bytes(arrivals.pop(0))
        return ""

    monkeypatch.setattr(spec, "download", download)
    want = hashlib.sha256(good).hexdigest()
    assert spec.fetch("x/pkg.zip", want, tmp_path).read_bytes() == good
    assert not (tmp_path / "pkg.zip.part").exists()
    arrivals[:] = [b"bad"] * spec.FETCH_TRIES
    (tmp_path / "pkg.zip").unlink()
    with pytest.raises(SystemExit, match="arrived damaged 3 times"):
        spec.fetch("x/pkg.zip", want, tmp_path)


def test_nup_pack_reads_back(tmp_path: Path) -> None:
    # a loose dump packed into one .nup (nup.py pack) reads back the same through the module the setup uses
    sys.path.insert(0, str(ROOT / "tools/prepare"))
    import nup

    src = tmp_path / ODD
    files = {"exefs/main": b"NSO0" + bytes(100), "romfs/a.bin": b"x" * 5000, "romfs/Sub Dir/b": b"yz", "romfs/c": b""}
    for name, data in files.items():
        (src / name).parent.mkdir(parents=True, exist_ok=True)
        (src / name).write_bytes(data)
    out = tmp_path / "g.nup"
    nup.pack(src, out, 0x0400C3F00006E000, 0)
    n = nup.Nup(out)
    assert n.title_id == "0400C3F00006E000" and n.version == 0
    got = {f"{part}/{k}": n.read(e) for part in ("exefs", "romfs") for k, e in n.files(part).items()}
    assert got == files


def test_copy_removes_other_versions_files(tmp_path: Path) -> None:
    # a folder set up from 1.0.0, then copied from 1.8.0: only 1.8.0's files stay (#261), never in a linked folder
    import gamecopy

    def make(root: Path, names: list[str]) -> None:
        for name in names:
            (root / name).parent.mkdir(parents=True, exist_ok=True)
            (root / name).write_text(name)

    src, dst = tmp_path / ODD / "1.8.0", tmp_path / ODD / "game/romfs"
    make(src, ["UI/Product_180.sarc.zs", "Sub Dir/b"])
    make(dst, ["UI/Product_100.sarc.zs", "Old/x/y", "Sub Dir/b"])
    gamecopy.copy_tree(src, dst, 2, "1.8.0")
    assert sorted(p.relative_to(dst).as_posix() for p in dst.rglob("*")) == ["Sub Dir", "Sub Dir/b", "UI",
                                                                             "UI/Product_180.sarc.zs"]  # fmt: skip
    gamecopy.clear_dir(dst, dst / "Sub Dir/b")  # the source inside: kept
    assert (dst / "UI/Product_180.sarc.zs").is_file()


def test_package_has_the_launchers_tools() -> None:
    # every script the start screen runs (setup_tool(exe_dir, "mod/import.py", ...)) is copied into the package by
    # tools/build/dist.py's SOURCES: until 13/11/2026 tools/mod was not, and the Mods page said "no setup tools"
    import ast
    import re

    tree = ast.parse((ROOT / "tools/build/dist.py").read_text(encoding="utf-8"))
    sources = next(ast.literal_eval(n.value) for n in tree.body
                   if isinstance(n, ast.Assign) and getattr(n.targets[0], "id", "") == "SOURCES")  # fmt: skip
    wanted = {"prepare/prepare.py"}
    for cpp in (ROOT / "runtime/ui/launcher").glob("*.cpp"):
        wanted |= set(re.findall(r'setup_tool\([^,]+, "([^"]+)"', cpp.read_text(encoding="utf-8")))
    assert wanted - {"prepare/prepare.py"}, "no setup_tool calls found: the pattern is stale"
    for rel in sorted(wanted):
        path = f"tools/{rel}"
        assert any(path == s or path.startswith(s.rstrip("/") + "/") for s in sources), f"{path} not in dist.py SOURCES"
        assert (ROOT / path).is_file(), path


def test_lastrun_reads_how_the_last_game_ended() -> None:
    # tools/prepare/lastrun.py: the start screen's notice; a wrong "known" sends a player away from a new bug
    import lastrun

    sigs = [{"issue": 22, "all": ["instruction pointer invalid"]},
            {"issue": 29, "all": ["spec.py", "args.cc.split"], "fixed": "v0.1.5"}]  # fmt: skip
    assert lastrun.verdict("x\n[host] gfx: window closed; exiting\n", "0.1.11", sigs) == "clean"
    assert lastrun.verdict("vk: device fault: Instruction pointer invalid\n", "0.1.11", sigs) == "known 22"
    assert lastrun.verdict("spec.py ... args.cc.split()\n", "0.1.4", sigs) == "fixed 29 v0.1.5"
    # on the release that fixed it, or later, the same text is a regression: still pointed to its issue
    assert lastrun.verdict("spec.py ... args.cc.split()\n", "0.1.5", sigs) == "known 29"
    assert lastrun.verdict("[host] FATAL: SIGSEGV at host 0x1\n", "0.1.11", sigs) == "crash"
    assert lastrun.verdict("[host] frame 300\n", "0.1.11", sigs) == ""   # killed or power off: nothing to say
    assert (ROOT / "tools/github/signatures.toml").is_file()


@pytest.mark.parametrize("text", ['{"meta": {', "{}"])
def test_broken_spec_not_reused(tmp_path: Path, text: str) -> None:
    """A spec the decoder rejects is built again, not reused by every later setup (issue #325)."""
    import shaderstep  # noqa: PLC0415

    broken = tmp_path / "instructions.json"
    broken.write_text(text)
    assert not shaderstep.spec_loads(broken)
    good = ROOT / "build/sm86/instructions.json"  # built on this machine, never in the repo
    if good.is_file():
        assert shaderstep.spec_loads(good)


def test_chunk_key_ignores_generated_headers_it_does_not_include(tmp_path: Path) -> None:
    """A new patch set rewrites the generated patches.h, which no chunk includes: it recompiled all 359 (16/11/2026)."""
    gen = tmp_path / "gen"
    (gen / "main").mkdir(parents=True)
    patches, layout = gen / "patches.h", gen / "layout.h"
    patches.write_text("#define RT_PATCH_COUNT 1\n")
    layout.write_text("#define A 1\n")
    chunk, uses = gen / "main/c0.c", gen / "main/c1.c"
    chunk.write_text('#include "cpu.h"\n')
    uses.write_text('#include "cpu.h"\n#include "layout.h"\n')
    keys = lambda: [gamecode.chunk_base(["true"], "cpu", [layout, patches], s) for s in (chunk, uses)]  # noqa: E731
    before = keys()
    patches.write_text("#define RT_PATCH_COUNT 2\n")
    assert keys() == before
    layout.write_text("#define A 2\n")
    after = keys()
    assert after[0] == before[0] and after[1] != before[1]


def test_ca_file_with_an_unreadable_windows_store(monkeypatch, tmp_path):
    """Issue #456: the Windows store holds a certificate Python cannot parse, the context raises OSError (ASN1) and
    the setup died before its first step; the pip bundle is used instead."""
    pem = tmp_path / "Lib/site-packages/pip/_vendor/certifi/cacert.pem"
    pem.parent.mkdir(parents=True)
    pem.write_text("")
    monkeypatch.delenv("SSL_CERT_FILE", raising=False)
    monkeypatch.setattr(sys, "prefix", str(tmp_path))

    def broken(*args, **kwargs):
        raise OSError(-2146881269, "invalid ASN1 tag")

    monkeypatch.setattr(prepare.update.ssl, "create_default_context", broken)
    assert prepare.update.ca_file() == str(pem)


def test_redo_exefs_if_missing(tmp_path: Path) -> None:
    # #419: the exefs step was skipped by its key while game/exefs had no main, and generate stopped on the layout
    state = {"done": {"exefs": "k", "romfs": "r"}}
    prepare.redo_exefs_if_missing(state, tmp_path)
    assert state["done"] == {"romfs": "r"}
    (tmp_path / "exefs").mkdir()
    (tmp_path / "exefs/main").write_text("x")
    state = {"done": {"exefs": "k"}}
    prepare.redo_exefs_if_missing(state, tmp_path)
    assert state["done"] == {"exefs": "k"}


def test_zig_caches_inside_the_game_folder(monkeypatch, tmp_path: Path) -> None:
    # #439: in the user's cache folder zig's tmp/ to o/ rename failed with CrossDevice on one file system
    monkeypatch.delenv("ZIG_GLOBAL_CACHE_DIR", raising=False)
    monkeypatch.delenv("ZIG_LOCAL_CACHE_DIR", raising=False)
    machine.use_own_zig_cache(tmp_path / "game")
    own = str(tmp_path / "game/setup-work/zig-cache")
    assert os.environ["ZIG_GLOBAL_CACHE_DIR"] == own and os.environ["ZIG_LOCAL_CACHE_DIR"] == own


def test_zig_cache_the_players_own(monkeypatch, tmp_path: Path) -> None:
    monkeypatch.setenv("ZIG_GLOBAL_CACHE_DIR", str(tmp_path / "cache"))
    machine.use_own_zig_cache(tmp_path / "game")
    assert os.environ["ZIG_GLOBAL_CACHE_DIR"] == str(tmp_path / "cache")


@pytest.mark.skipif(not shutil.which("cc"), reason="no C compiler")
def test_damaged_generated_c_raises_to_regenerate(tmp_path: Path) -> None:
    # #434: `uint6<NUL>_t` in a generated chunk; generate was skipped by its key, so every retry failed the same
    src, out, stamp = tmp_path / "main_0203.c", tmp_path / "a.o", tmp_path / "a.key"
    src.write_bytes(b"typedef unsigned long long uint64_t;\nuint6\0_t x;\n")
    with pytest.raises(gamecode.DamagedSource, match="NUL bytes"):
        gamecode.compile_one(["cc"], [], src, out, stamp, "k")
    src.write_text("int x = ;\n")  # an ordinary error stays a message
    assert gamecode.compile_one(["cc"], [], src, out, stamp, "k").startswith("main_0203.c: ")
    src.write_text("int x = 1;\n")
    assert gamecode.compile_one(["cc"], [], src, out, stamp, "k") == "" and stamp.read_text() == "k"
