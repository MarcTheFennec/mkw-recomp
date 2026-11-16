"""Assemble the player's folder: the runtime and its libraries, and setup/ with the tools that build the game on the
player's machine. Nothing of the game goes in; the start screen's setup adds it.

Why: the player has no Python, compiler or glslc. setup/ carries a portable CPython (python-build-standalone) with numpy
and lz4, zig as the C compiler, glslc from conda-forge (Google's own shaderc builds are not downloadable), and the
sources the setup runs (recomp/, sm86/, tools/, the runtime headers the game code includes). NVIDIA's SM86 spec does
not go in: the setup builds it (sm86/spec/spec.py). Every download is pinned by sha256, as tools/build/windeps.py; a
mismatch stops.

Layout of build/dist/<target>/ (after build.py --dist):
    mkw, mkwgame.id                 the runtime (mkw.exe and its DLLs on Windows; mkwgame.* comes from the setup)
    setup/python, setup/zig, setup/glslc
    setup/src/                      recomp/, sm86/, tools/{build,prepare,romfs,shaders}, runtime/{cpu,core/boot/gamelib.h}
    setup/lib/libmkw.dll.a          (Windows) the runtime's import library the game code library links against
    setup/lib/crt/                  (Windows) zig's C runtime for the target, prebuilt, and link.json: windows_crt()
The desktop shortcut is the start screen's (a .desktop on Linux, a .lnk on Windows: platform.h).

Usage: uv run tools/build/dist.py [--target linux|linux-aarch64|windows] [--check]
       linux-aarch64: the AArch64 Linux package (after build.py --dist --arch aarch64), assembled on any machine.
       --check: list what would be fetched and copied, change nothing.
"""

import argparse
import hashlib
import json
import os
import platform
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DL = ROOT / "build/dist/dl"
CONDA = "https://conda.anaconda.org/conda-forge/"
PBS = "https://github.com/astral-sh/python-build-standalone/releases/download/20261026/"
PYPI = "https://files.pythonhosted.org/packages/"
# target: name -> (url, sha256)
PINS = {
    "linux": {
        "python": (PBS + "cpython-3.14.10%2B20261026-x86_64-unknown-linux-gnu-install_only_stripped.tar.gz",
                   "4d3c16c59ed0802a3d1d34d8bce504b15ceeaff39d62dadecc6a9fa803412aed"),
        "zig": ("https://ziglang.org/download/0.17.1/zig-x86_64-linux-0.17.1.tar.xz",
                "3581863e3cbcfd0fce0efc682ce7e9e93da96668a977791d356a26d2a7e9b17a"),
        "numpy": (PYPI + "16/d5/928078d2b28f26829b138b4a6c3980045022fb409f570657a224ae60ef4e/"
                  "numpy-2.5.4-cp314-cp314-manylinux_2_27_x86_64.manylinux_2_28_x86_64.whl",
                  "d549420b8858885cea8838a727842249218b9c1da24dd517e25c9c7a948310a3"),
        "lz4": (PYPI + "f1/01/d52c7b11eaa286d49dae619c0eec4aabc0bf3cda7a7467eb77c62c4471f3/"
                "lz4-4.4.5-cp314-cp314-manylinux2014_x86_64.manylinux_2_17_x86_64.manylinux_2_28_x86_64.whl",
                "0bba042ec5a61fa77c7e380351a61cb768277801240249841defd2ff0a10742f"),
        "shaderc": (CONDA + "linux-64/shaderc-2026.4-hcebf71c_1.conda",
                    "5cbe27520e948983863fd2f2ce59b2585d261430875c42c51a2dec823806694b"),
        "glslang": (CONDA + "linux-64/glslang-16.6.0-h980caa0_0.conda",
                    "390b9e94b29b53366673e5460b433c9daa34788f5cdabf8b30bf40cf086c60dd"),
        "spirv-tools": (CONDA + "linux-64/spirv-tools-2026.4-h7148c6a_2.conda",
                        "e8791349946b2c98ffa5cc67cf1525931e066aef8a01800d6f7c151444a3f052"),
        "libstdcxx": (CONDA + "linux-64/libstdcxx-16.2.0-h934c35e_7.conda",
                      "fa7018298629fc429971fffd6ecba3d27a7b02e4e890d6a93ea01ffebe8cb745"),
        "libgcc": (CONDA + "linux-64/libgcc-16.2.0-ha9f2e26_7.conda",
                   "e031634c3a928f9594eba868bc46ef5230d6571a364a0c63215b06c4563bfcea"),
    },
    "linux-aarch64": {
        "python": (PBS + "cpython-3.14.10%2B20261026-aarch64-unknown-linux-gnu-install_only_stripped.tar.gz",
                   "90f48337bc3dd4ae6eda81e001e4c981236d403b6b5a07778bbdf94ee423dc00"),
        "zig": ("https://ziglang.org/download/0.17.1/zig-aarch64-linux-0.17.1.tar.xz",
                "a1546d5646eb184437918d6635ca6b762a0e889eac09cef720ee7a1ed3fe7795"),
        "numpy": (PYPI + "63/d6/34b0a2b0741386a63025a65a2c09caaaaaad6d0ca95b66cd65c30dd7fcb5/"
                  "numpy-2.5.4-cp314-cp314-manylinux_2_27_aarch64.manylinux_2_28_aarch64.whl",
                  "4054173604cd8658796053f1f3bc0befb68ec1c0762c57fdad61e199256a8617"),
        "lz4": (PYPI + "67/79/a4e91872ab60f5e89bfad3e996ea7dc74a30f27253faf95865771225ccba/"
                "lz4-4.4.5-cp314-cp314-manylinux2014_aarch64.manylinux_2_17_aarch64.manylinux_2_28_aarch64.whl",
                "3b84a42da86e8ad8537aabef062e7f661f4a877d1c74d65606c49d835d36d668"),
        "shaderc": (CONDA + "linux-aarch64/shaderc-2026.4-h2d14b02_1.conda",
                    "ec6a66f8c593745e8f4aca9ba0400e7a43fdd1a36257fdb4464c083075770259"),
        "glslang": (CONDA + "linux-aarch64/glslang-16.6.0-ha162a40_0.conda",
                    "095a5c3efa9f5e473899cbf36463f9024b02f0763321cfd605d289516b2dee39"),
        "spirv-tools": (CONDA + "linux-aarch64/spirv-tools-2026.4-hde6636f_2.conda",
                        "d058bbe500b0ef6c894762a459b51cf538d8012c1083b1fa373b3a9432f91f91"),
        "libstdcxx": (CONDA + "linux-aarch64/libstdcxx-16.2.0-hef695bb_7.conda",
                      "b241a3acfa9cdb1359112864a1241225533c92642de3000d52ae65ef5d3a9786"),
        "libgcc": (CONDA + "linux-aarch64/libgcc-16.2.0-h205dda4_7.conda",
                   "eae7a3449d5a3b5351c5427137c076cc3fc826645c7bfc42dc10772b783826f1"),
    },
    "windows": {
        "python": (PBS + "cpython-3.14.10%2B20261026-x86_64-pc-windows-msvc-install_only_stripped.tar.gz",
                   "c1876a9795d3a94ee49a1499a69cd91e26ca3e2829593f93fa6e4db90d05402b"),
        "zig": ("https://ziglang.org/download/0.17.1/zig-x86_64-windows-0.17.1.zip",
                "b03f477a1943673e02aca0f570e8030dffd536ce1f45cbc4d3326ec31159562e"),
        "numpy": (PYPI + "52/9a/e1e512ebc948d5b9dd33b08736760f0ebbed2848fd4eda1f553088a6dcee/"
                  "numpy-2.5.4-cp314-cp314-win_amd64.whl",
                  "2ca144f15135b6212a5c47b1e2aeca6e412f102f95a2d5d88d8aec77eb255de3"),
        "lz4": (PYPI + "18/2c/8332080fd293f8337779a440b3a143f85e374311705d243439a3349b81ad/"
                "lz4-4.4.5-cp314-cp314-win_amd64.whl",
                "92159782a4502858a21e0079d77cdcaade23e8a5d252ddf46b0652604300d7be"),
        "shaderc": (CONDA + "win-64/shaderc-2026.4-hef10606_1.conda",
                    "d73088bb4b4fc388252d53c1aba4522891c131b7020815198776668e47616caa"),
        "glslang": (CONDA + "win-64/glslang-16.6.0-h8fa7867_0.conda",
                    "048f538b07594cc717a14517191a81cb1d3825fdde9c573d132c158b26d15806"),
        "spirv-tools": (CONDA + "win-64/spirv-tools-2026.4-h49e36cd_2.conda",
                        "933e40fe9ca887a63ef742260f0ed801cda317354806504d864764a4ed9749e1"),
        "vc14_runtime": (CONDA + "win-64/vc14_runtime-14.51.36247-habf1de7_42.conda",
                         "8dfbfd68ebf53249d11e528cbb80ad5a2460b0c5b4636eed82ee0d3950827a3d"),
    },
}  # fmt: skip
# The sources the setup runs, from the repository (tools/prepare/prepare.py imports and runs them).
# tools/mod: the Mods page's import, merge, GameBanana and .ips tools (the launcher runs them; tests/test_setup.py checks
# every tool it names is here: until 13/11/2026 none was, and the page failed with "no setup tools" in every package)
SOURCES = ["recomp", "sm86", "tools/build/build.py", "tools/prepare", "tools/mod", "tools/romfs/resolution.py",
           "tools/i18n/catalog.py", "tools/i18n/game_texts.py", "i18n/game",
           "tools/shaders/extract.py", "tools/shaders/translate.py", "tools/shaders/shaderc_lib.py",
           "runtime/cpu",
           "runtime/core/boot/gamelib.h",
           "tools/github/signatures.toml"]  # fmt: skip  (the known errors tools/prepare/lastrun.py names)
# Windows DLLs mkw.exe needs beside it (build/win/deps/bin, tools/build/windeps.py); vulkan-1.dll is the driver's.
WIN_DLLS = ["SDL3.dll", "libopus-0.dll", "libusb-1.0.dll"]   # libusb: SDL3.dll loads it (windeps.py)


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def fetch(url: str, want: str, name: str = "") -> Path:
    """url into build/dist/dl (as name when given: files of the same name from different places), checked."""
    path = DL / (name or url.rsplit("/", 1)[1].replace("%2B", "+"))
    if not path.is_file():
        print(f"get {url}", flush=True)
        part = path.with_suffix(path.suffix + ".part")
        with urllib.request.urlopen(url) as r, part.open("wb") as f:
            shutil.copyfileobj(r, f)
        part.rename(path)
    got = sha256(path)
    if got != want:
        sys.exit(f"{path.name}: sha256 {got}, expected {want}")
    return path


def unpack_top(archive: Path, dst: Path) -> None:
    """An archive with one top folder (python/, zig-.../) into dst, that folder's contents at dst's root."""
    tmp = dst.with_name(dst.name + ".tmp")
    shutil.rmtree(tmp, ignore_errors=True)
    if archive.suffix == ".zip":
        with zipfile.ZipFile(archive) as z:
            z.extractall(tmp)
    else:
        with tarfile.open(archive) as t:
            t.extractall(tmp, filter="tar")
    (top,) = tmp.iterdir()
    shutil.rmtree(dst, ignore_errors=True)
    top.rename(dst)
    tmp.rmdir()


def unpack_conda(archive: Path, dst: Path) -> None:
    """A .conda package (a zip holding pkg-*.tar.zst) into dst; Python's zstd module reads it (3.14)."""
    from compression import zstd

    with zipfile.ZipFile(archive) as z:
        (inner,) = [n for n in z.namelist() if n.startswith("pkg-")]
        with z.open(inner) as raw, zstd.open(raw) as zf, tarfile.open(fileobj=zf, mode="r|") as t:
            t.extractall(dst, filter="tar")


def unpack_wheel(wheel: Path, site: Path) -> None:
    with zipfile.ZipFile(wheel) as z:
        z.extractall(site)


def windows_crt(crt: Path) -> None:
    """zig's C runtime for x86_64-windows-gnu (mingw's startup object, zigc, compiler_rt, the import libraries of
    kernel32, the UCRT...) built here once, with the flags of zig's own lld-link line in link.json, so the setup links
    with zig lld-link alone (prepare.py). zig builds them on a machine's first link, and on many players' Windows PCs that
    build never ended (issue #2: stuck at 359/360 on 0.1.2 and 0.1.3, no "link probe" line in setup.log after 30 min;
    84 s on our 6-thread PC). They depend only on the target and zig's version: the Linux zig of the same release builds
    them."""
    host = DL / "zig-host"
    # built on Windows itself (build.py HOST_WINDOWS): the Windows zig of the same release builds the same C runtime
    archive = fetch(*PINS["windows" if os.name == "nt" else "linux"]["zig"])
    stamp = host / ".from"
    # unpacked once per zig version: a fresh unpack's file times make zig's cache build everything again (41 s)
    if not stamp.exists() or stamp.read_text() != archive.name:
        unpack_top(archive, host)
        stamp.write_text(archive.name)
    shutil.rmtree(crt, ignore_errors=True)
    crt.mkdir(parents=True)
    with tempfile.TemporaryDirectory() as tmp:
        t = Path(tmp)
        (t / "probe.c").write_text("int probe(void) { return 0; }\n")
        # zig's cache next to the downloads, kept between runs (release.yml caches build/dist/dl): building the C
        # runtime was 2 of the Windows job's 4 minutes on a fresh runner, seconds when the cache has it
        env = {**os.environ, "ZIG_GLOBAL_CACHE_DIR": str(DL / "zig-crt-cache"), "ZIG_LIB_DIR": str(host / "lib"),
               "ZIG_VERBOSE_LINK": "1"}  # fmt: skip
        r = subprocess.run([str(host / ("zig.exe" if os.name == "nt" else "zig")), "cc", "-target", "x86_64-windows-gnu", "-shared", "-o",
                            str(t / "probe.dll"), str(t / "probe.c")],
                           env=env, capture_output=True, text=True, check=True)  # fmt: skip
        (line,) = [ln.split() for ln in (r.stdout + r.stderr).splitlines() if ln.startswith("lld-link ")]
        flags, files = [], {}
        for arg in line[1:]:
            p = Path(arg)   # relative when zig's cache is under the working directory (build/dist/dl)
            if p.suffix in (".obj", ".lib") and (p.is_absolute() or p.exists()):
                p = p.resolve()
                if p.stem == "probe" or files.get(p.name) == p:
                    continue
                if p.name in files:
                    sys.exit(f"zig's link line has two different {p.name}: {files[p.name]}, {p}")
                shutil.copy2(p, crt / p.name)
                files[p.name] = p
            elif not arg.startswith(("-PDB:", "-PDBALTPATH:", "-OUT:", "-IMPLIB:")):
                flags.append(arg)
    (crt / "link.json").write_text(json.dumps({"flags": flags, "files": list(files)}, indent=1))
    print(f"{crt}: {len(files)} files, flags {' '.join(flags)}")


def copy_sources(src_root: Path) -> None:
    shutil.rmtree(src_root, ignore_errors=True)
    skip = shutil.ignore_patterns("__pycache__", "*.pyc", "README.md")
    for rel in SOURCES:
        s, d = ROOT / rel, src_root / rel
        d.parent.mkdir(parents=True, exist_ok=True)
        if s.is_dir():
            shutil.copytree(s, d, ignore=skip)
        else:
            shutil.copy2(s, d)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--target", choices=tuple(PINS), default="linux")
    ap.add_argument("--check", action="store_true", help="list what would be fetched and copied; change nothing")
    args = ap.parse_args()
    out = ROOT / "build/dist" / args.target
    exe = out / ("mkw.exe" if args.target == "windows" else "mkw")
    if args.check:
        for name, (url, want) in PINS[args.target].items():
            print(f"{name}: {url} {want}")
        print("sources:", " ".join(SOURCES))
        return
    if not exe.is_file():
        how = "--arch aarch64" if args.target == "linux-aarch64" else f"--target {args.target}"
        sys.exit(f"{exe} missing: uv run tools/build/build.py --dist {how} first")
    DL.mkdir(parents=True, exist_ok=True)
    pins = PINS[args.target]
    got = {name: fetch(url, want) for name, (url, want) in pins.items()}
    setup = out / "setup"
    setup.mkdir(exist_ok=True)
    unpack_top(got["python"], setup / "python")
    site = setup / "python" / ("Lib/site-packages" if args.target == "windows" else "lib/python3.14/site-packages")
    for wheel in ("numpy", "lz4"):
        unpack_wheel(got[wheel], site)
    unpack_top(got["zig"], setup / "zig")
    glslc = setup / "glslc"
    shutil.rmtree(glslc, ignore_errors=True)
    for name in ("shaderc", "glslang", "spirv-tools", "libstdcxx", "libgcc", "vc14_runtime"):
        if name in got:
            unpack_conda(got[name], glslc)
    if args.target == "windows":
        # glslc.exe and the DLLs it loads, side by side (conda-forge's Library/bin)
        lib_bin = glslc / "Library/bin"
        for p in lib_bin.iterdir():
            if p.suffix in (".exe", ".dll"):
                shutil.copy2(p, glslc / p.name)
        (setup / "lib").mkdir(exist_ok=True)
        shutil.copy2(ROOT / "build/obj-dist-win/libmkw.dll.a", setup / "lib/libmkw.dll.a")
        windows_crt(setup / "lib/crt")
        for dll in WIN_DLLS:
            shutil.copy2(ROOT / "build/win/deps/bin" / dll, out / dll)
    copy_sources(setup / "src")
    size = sum(p.stat().st_size for p in out.rglob("*") if p.is_file() and not p.is_symlink())
    print(f"{out}: {size / 2**20:.0f} MiB")
    if args.target == "linux" or (args.target == "linux-aarch64" and platform.machine() in ("aarch64", "arm64")):
        # the package's tools run here: a quick proof before a setup relies on them
        subprocess.run([str(setup / "python/bin/python3"), "-I", "-c", "import numpy, lz4.block"], check=True)
        subprocess.run([str(setup / "zig/zig"), "version"], check=True)


if __name__ == "__main__":
    main()
