"""Pack a release from the player's folder (build/dist/<target>, after build.py --dist --runtime-only and dist.py): the
Linux AppImage or the Windows zip. Only the runtime, its libraries and setup/ go in, never anything of the game: a dev
folder that also holds the built game (mkwgame.*, spirv/, the caches) packs the same release.

Linux: the AppImage's AppRun keeps a copy of mkw in ${XDG_DATA_HOME:-~/.local/share}/mkw-recomp, updated when the
AppImage brings a new one, links setup/ there to the mount (a new path on every start) and starts it with the bundled
libraries first on the library path. The runtime keeps using the folder of its executable for the game the setup builds,
its settings and caches, as in the Windows zip; the read-only mount only holds the tools. The libraries come from the
sysroot the release links against (tools/build/sysroot.py), which lets it start on systems as old as Debian 11. The
packing follows totk-pc-recomp's tools/make_release_linux.py (MIT): appimagetool and the static type-2 runtime from
release tags (no libfuse2 needed on the player's system), pinned by sha256.

Usage: uv run tools/build/release.py --target linux|windows --version 0.1.0
       writes build/release/mkw-recomp-<version>-x86_64.AppImage or mkw-recomp-<version>-windows-x64.zip
"""

import argparse
import os
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from dist import WIN_DLLS, fetch  # noqa: E402
from sysroot import SYSROOT  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "build/release"
PINS = {
    "appimagetool": ("https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage",
                     "ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0"),
    "runtime": ("https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64",
                "2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d"),
}  # fmt: skip
# Libraries every desktop has and the AppImage must take from it: glibc, the Vulkan loader (it finds the system's
# drivers), libgcc_s and libstdc++ (the sysroot's is older than any desktop's, and Mesa's drivers need the newer one).
SYSTEM_LIBS = {"libc.so.6", "libm.so.6", "libdl.so.2", "libpthread.so.0", "librt.so.1", "ld-linux-x86-64.so.2",
               "libvulkan.so.1", "libgcc_s.so.1", "libstdc++.so.6"}  # fmt: skip
# The licence files of the libraries bundled (the sysroot's, Debian's copyright files) and of the static zstd.
LICENSES = {"SDL3": "usr/local/share/doc/SDL3/LICENSE.txt", "libopus": "usr/share/doc/libopus0/copyright",
            "libvpx": "usr/share/doc/libvpx6/copyright", "zstd": "usr/local/share/doc/zstd/LICENSE"}  # fmt: skip
GAME_MARKER = "MKW_RECOMP_" + "GENERATED_FROM_GAME_CODE"

APPRUN = """#!/bin/sh
# The AppImage's entry (tools/build/release.py): the runtime runs from a copy in the player's data folder, which holds
# the game the setup builds; setup/ and the libraries stay in the mount.
set -eu
here=$(dirname "$(readlink -f "$0")")
home=${XDG_DATA_HOME:-$HOME/.local/share}/mkw-recomp
mkdir -p "$home"
if ! cmp -s "$here/mkw" "$home/mkw"; then
    # a rename, not a write over it: a mkw still running keeps its old file
    cp "$here/mkw" "$home/mkw.new"
    mv -f "$home/mkw.new" "$home/mkw"
fi
ln -sfn "$here/setup" "$home/setup"
export LD_LIBRARY_PATH="$here/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$home/mkw" "$@"
"""

DESKTOP = """[Desktop Entry]
Type=Application
Name=mkw-recomp
Comment=Static recompilation of Mario Kart World; builds the game from your own copy
Exec=AppRun
Icon=mkw-recomp
Categories=Game;
Terminal=false
"""


def needed(elf: Path) -> list[str]:
    out = subprocess.run(["readelf", "-d", str(elf)], capture_output=True, text=True, check=True).stdout
    return [line.split("[", 1)[1].rstrip("]") for line in out.splitlines() if "(NEEDED)" in line]


def find_lib(name: str) -> Path:
    for d in ("usr/local/lib", "usr/lib/x86_64-linux-gnu", "lib/x86_64-linux-gnu"):
        if (SYSROOT / d / name).exists():
            return (SYSROOT / d / name).resolve()
    sys.exit(f"{name}: not in {SYSROOT} (built with build.py --sysroot?)")


def bundle_libs(exe: Path, lib: Path) -> None:
    """The libraries mkw needs that are not SYSTEM_LIBS, and theirs, from the sysroot."""
    lib.mkdir()
    todo = [n for n in needed(exe) if n not in SYSTEM_LIBS]
    while todo:
        name = todo.pop()
        if (lib / name).exists():
            continue
        src = find_lib(name)
        shutil.copy2(src, lib / name)
        todo += [n for n in needed(src) if n not in SYSTEM_LIBS]
    print("bundled:", " ".join(sorted(p.name for p in lib.iterdir())))


def check_no_game(root: Path) -> None:
    """The generated C carries GAME_MARKER (recomp/gen.py); setup/src is the repository's sources. NVIDIA's SM86 spec
    neither: the player's setup builds it (sm86/spec/spec.py)."""
    for p in root.rglob("*"):
        if p.name in ("instructions.json", "sm_86_instructions.txt"):
            sys.exit(f"{p}: NVIDIA's SM86 spec in the release")
        if p.is_file() and not p.is_symlink() and p.name != "gen.py" and GAME_MARKER.encode() in p.read_bytes():
            sys.exit(f"{p}: game code in the release")


def licenses(dst: Path) -> None:
    dst.mkdir()
    shutil.copy2(ROOT / "LICENSE", dst / "mkw-recomp-LICENSE")
    for name, rel in LICENSES.items():
        shutil.copy2(SYSROOT / rel, dst / f"{name}-copyright")


def linux(dist: Path, version: str) -> Path:
    exe = dist / "mkw"
    glibc = subprocess.run(["objdump", "-T", str(exe)], capture_output=True, text=True, check=True).stdout
    newest = max((tuple(map(int, v.split("."))) for v in set(_glibc_versions(glibc))), default=(0, 0))
    if newest > (2, 31):  # built against the machine's glibc: the AppImage would need that glibc or newer
        sys.exit(f"{exe} needs glibc {'.'.join(map(str, newest))}: build.py --sysroot build/sysroot")
    app = OUT / "AppDir"
    shutil.rmtree(app, ignore_errors=True)
    app.mkdir(parents=True)
    shutil.copy2(exe, app / "mkw")
    shutil.copytree(dist / "setup", app / "setup", symlinks=True)
    bundle_libs(exe, app / "lib")
    licenses(app / "licenses")
    check_no_game(app)
    (app / "AppRun").write_text(APPRUN)
    (app / "AppRun").chmod(0o755)
    (app / "mkw-recomp.desktop").write_text(DESKTOP)
    subprocess.run([sys.executable, str(ROOT / "tools/build/icon.py"), str(app / "mkw-recomp.png")], check=True)
    (app / ".DirIcon").symlink_to("mkw-recomp.png")
    tool, runtime = fetch(*PINS["appimagetool"]), fetch(*PINS["runtime"])
    tool.chmod(0o755)
    out = OUT / f"mkw-recomp-{version}-x86_64.AppImage"
    # appimagetool is an AppImage itself: extracted and run, no FUSE in a container. Into a .part then renamed: an
    # AppImage of the same name still mounted (a test of it) made mksquashfs fail with "Text file busy" (09/11/2026).
    env = dict(os.environ, ARCH="x86_64", APPIMAGE_EXTRACT_AND_RUN="1")
    part = out.with_name(out.name + ".part")
    subprocess.run([str(tool), "--no-appstream", "--runtime-file", str(runtime), str(app), str(part)], env=env,
                   check=True)  # fmt: skip
    part.replace(out)
    shutil.rmtree(app)
    return out


def _glibc_versions(text: str) -> list[str]:
    return [w.removeprefix("(GLIBC_").rstrip(")") for w in text.split() if w.startswith("(GLIBC_2.")]


def windows(dist: Path, version: str) -> Path:
    out = OUT / f"mkw-recomp-{version}-windows-x64.zip"
    files = [dist / "mkw.exe", *(dist / d for d in WIN_DLLS), *sorted((dist / "setup").rglob("*"))]
    check_no_game(dist / "setup")
    top = Path("mkw-recomp")
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for p in files:
            if p.is_file():
                z.write(p, top / p.relative_to(dist))
        z.write(ROOT / "LICENSE", top / "licenses/mkw-recomp-LICENSE")
        # the libraries in mkw.exe and beside it (tools/build/windeps.py keeps each package's licence files)
        for p in sorted((ROOT / "build/win/deps/share/licenses").rglob("*")):
            if p.is_file():
                z.write(p, top / "licenses" / p.relative_to(ROOT / "build/win/deps/share/licenses"))
    return out


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--target", choices=("linux", "windows"), default="linux")
    ap.add_argument("--version", required=True, help="the release's version, e.g. 0.1.0 (the tag without its v)")
    args = ap.parse_args()
    dist = ROOT / "build/dist" / args.target
    if not (dist / "setup").is_dir():
        sys.exit(f"{dist}/setup missing: uv run tools/build/dist.py --target {args.target} first")
    OUT.mkdir(parents=True, exist_ok=True)
    version = args.version.replace("/", "-")
    out = (linux if args.target == "linux" else windows)(dist, version)
    print(f"{out}: {out.stat().st_size / 2**20:.0f} MiB")


if __name__ == "__main__":
    main()
