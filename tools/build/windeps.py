"""Fetch what the Windows build needs into build/win: llvm-mingw (clang + lld for x86_64-w64-mingw32, UCRT) and the
headers, import libraries and DLLs of SDL3, libusb, Opus, libvpx, zstd, Vulkan, VMA and xxHash.

Why: tools/build/build.py --target windows builds the Windows exe with llvm-mingw plus the MSYS2 UCRT64 packages, the
way totk-pc-recomp builds its installer with --toolchain (pins taken from its tools/make_release.py, MIT). It runs on
Linux (a cross build, llvm-mingw's Linux build) or on Windows itself (llvm-mingw's Windows build, a .zip); the
libraries are the same. SDL3 is built here from its source release with that toolchain: the upstream mingw package
(and MSYS2's) has no libusb, and SDL starts the Switch 2 controllers over USB only through it (it sends their start-up
commands on the second, non-HID interface; discussion #113). Built with SDL_HIDAPI_LIBUSB_SHARED, SDL3.dll loads
libusb-1.0.dll (MSYS2, LGPL-2.1, beside it) when present. Every download is checked against its sha256; a mismatch stops.

Layout:
    build/win/llvm-mingw/   the toolchain (bin/x86_64-w64-mingw32-clang)
    build/win/deps/         include/, lib/ (import and static libraries), bin/ (the DLLs mkw.exe loads)

Usage: uv run tools/build/windeps.py [--dry-run]   (downloads cached in build/win/dl; ~250 MB unpacked; SDL needs cmake
and ninja, ~10 s on 16 threads; on Windows, when they are not on PATH, they are taken from MSYS2 UCRT64:
C:/msys64/ucrt64/bin or MSYS2_UCRT64_BIN, as build.py does)
"""

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WIN = ROOT / "build/win"
DL = WIN / "dl"
MSYS2 = "https://repo.msys2.org/mingw/ucrt64/"
LLVM_VERSION = "20261020"  # clang 23, as totk-pc-recomp's release
SDL_VERSION = "3.4.18"
HOST_WINDOWS = os.name == "nt"  # build.py on Windows itself: llvm-mingw's Windows build instead of its Linux one
MSYS2_UCRT64_BIN = Path(os.environ.get("MSYS2_UCRT64_BIN", "C:/msys64/ucrt64/bin"))  # ninja (and cmake) on Windows
# name: (url, sha256)
PINS = {
    "llvm-mingw": (
        (
            f"https://github.com/mstorsjo/llvm-mingw/releases/download/{LLVM_VERSION}/"
            f"llvm-mingw-{LLVM_VERSION}-ucrt-ubuntu-22.04-x86_64.tar.xz"
        ),
        "55064b60b991a1e1e8090b8c2f84e82b22cfe5903d12c159d65064cf5e8ae335",
    ),
    # The same llvm-mingw built for Windows, when the build runs on Windows itself (build.py HOST_WINDOWS)
    "llvm-mingw-windows": (
        f"https://github.com/mstorsjo/llvm-mingw/releases/download/{LLVM_VERSION}/llvm-mingw-{LLVM_VERSION}-ucrt-x86_64.zip",
        "fd5462b7f5d93b2ff981fcd3f9c7e427e47f3203c64ea91c27bcc444eebd74cd",
    ),
    "sdl3-src": (
        f"https://github.com/libsdl-org/SDL/releases/download/release-{SDL_VERSION}/SDL3-{SDL_VERSION}.tar.gz",
        "9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3",
    ),
    "libusb": (
        MSYS2 + "mingw-w64-ucrt-x86_64-libusb-1.0.30-1-any.pkg.tar.zst",
        "20106e6ff31b4581bb111b28a68b6b79a50dc3e5fc3033944e1c4b3ff8cd812e",
    ),
    "opus": (
        MSYS2 + "mingw-w64-ucrt-x86_64-opus-1.6.1-2-any.pkg.tar.zst",
        "bb114bcf26225f50f874497ebfd6b8224fff2a20bda3f8ee45cd07ce5f02657b",
    ),
    "libvpx": (
        MSYS2 + "mingw-w64-ucrt-x86_64-libvpx-1.17.0-1-any.pkg.tar.zst",
        "8ff6919dbb88ea26ce28d22fd81d6f0d6e200cf56a4dd81ffc2d33124fe98024",
    ),
    "libvpx-license": (  # the package carries none; mkw.exe links libvpx statically and the zip carries its BSD notice
        "https://raw.githubusercontent.com/webmproject/libvpx/v1.17.0/LICENSE",
        "8267348d5af1262c11d1a08de2f5afc77457755f1ac658627dd9acf71011d615",
    ),
    "vulkan-headers": (
        MSYS2 + "mingw-w64-ucrt-x86_64-vulkan-headers-1~1.4.363.0-1-any.pkg.tar.zst",
        "be602b2657cace531316e78b1eda4087e8f7f84ebb877f08fba198a2db06406a",
    ),
    "vulkan-loader": (
        MSYS2 + "mingw-w64-ucrt-x86_64-vulkan-loader-1~1.4.363.0-1-any.pkg.tar.zst",
        "065076fe13d39ab97eea4740596426d5ae741b5044e48e1c9495d04835c92dbe",
    ),
    "vma": (
        MSYS2 + "mingw-w64-ucrt-x86_64-vulkan-memory-allocator-3.4.0-1-any.pkg.tar.zst",
        "44c81162337915afecda3185f66fd98268a54454580b2717080c9dfef57fe781",
    ),
    "xxhash": (  # header only (XXH_INLINE_ALL in runtime/gfx/vk/tex/vk_texpre.c); 0.8.4 as Arch's
        MSYS2 + "mingw-w64-ucrt-x86_64-xxhash-0.8.4-1-any.pkg.tar.zst",
        "a22d39e85edd6f0b4c7fe34733d28f17d49715f6c8dea67c142161c02bfbbde2",
    ),
    "zstd": (
        MSYS2 + "mingw-w64-ucrt-x86_64-zstd-1.5.7-2-any.pkg.tar.zst",
        "dbdb8427280046a2b41697780aa4c52983b708082b0da4755951dc3bea96ca89",
    ),
}
# vulkan-1.dll is the system's (the GPU driver installs it): only its import library is kept.
SKIP_DLLS = {"vulkan-1.dll"}


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def fetch(name: str) -> Path:
    url, want = PINS[name]
    path = DL / url.rsplit("/", 1)[1].replace("%7E", "~")
    if not path.is_file():
        print(f"get {url}")
        part = path.with_suffix(path.suffix + ".part")
        with urllib.request.urlopen(url) as r, part.open("wb") as f:
            shutil.copyfileobj(r, f)
        part.rename(path)
    got = sha256(path)
    if got != want:
        sys.exit(f"{path.name}: sha256 {got}, expected {want}")
    return path


def unpack_llvm(archive: Path) -> None:
    dst = WIN / "llvm-mingw"
    stamp = dst / ".version"
    version = f"{LLVM_VERSION} {archive.name}"  # which host's build: a Linux unpack is not reused on Windows
    if stamp.is_file() and stamp.read_text() in (version, None if HOST_WINDOWS else LLVM_VERSION):
        return
    shutil.rmtree(dst, ignore_errors=True)
    tmp = WIN / "llvm-mingw.tmp"
    shutil.rmtree(tmp, ignore_errors=True)
    if archive.suffix == ".zip":
        with zipfile.ZipFile(archive) as z:
            z.extractall(tmp)
    else:
        with tarfile.open(archive) as t:
            t.extractall(tmp, filter="tar")
    (top,) = tmp.iterdir()
    top.rename(dst)
    tmp.rmdir()
    stamp.write_text(version)


def unpack_deps(archives: dict[str, Path]) -> None:
    """Each MSYS2 package's include/, lib/, bin/*.dll and share/licenses/ (under ucrt64/) into build/win/deps; the
    licences go into the release zip (tools/build/release.py)."""
    dst = WIN / "deps"
    shutil.rmtree(dst, ignore_errors=True)
    prefix = "ucrt64/"
    for archive in archives.values():
        with tarfile.open(archive) as t:
            for m in t.getmembers():
                rel = m.name.split(prefix, 1)[1] if prefix in m.name else ""
                top = rel.split("/", 1)[0]
                if not m.isfile() or (top not in ("include", "lib", "bin") and not rel.startswith("share/licenses/")):
                    continue
                if top == "bin" and (not rel.endswith(".dll") or Path(rel).name in SKIP_DLLS):
                    continue
                out = dst / rel
                out.parent.mkdir(parents=True, exist_ok=True)
                with t.extractfile(m) as src, out.open("wb") as f:
                    shutil.copyfileobj(src, f)


def sdl_tools() -> None:
    """cmake and ninja for build_sdl(), checked before any download. On Windows, MSYS2 UCRT64's bin goes at the end of
    PATH when one of them is missing, as build.py does for ninja: what PATH already has stays first."""
    tools = ("cmake", "ninja")
    if HOST_WINDOWS and not all(map(shutil.which, tools)) and MSYS2_UCRT64_BIN.is_dir():
        os.environ["PATH"] = f"{os.environ['PATH']}{os.pathsep}{MSYS2_UCRT64_BIN}"
    missing = [t for t in tools if not shutil.which(t)]
    if missing:
        where = f" (on PATH or in {MSYS2_UCRT64_BIN}, MSYS2_UCRT64_BIN)" if HOST_WINDOWS else ""
        sys.exit(f"{' and '.join(missing)} not found{where}: SDL3 builds with CMake and Ninja (tools/build/README.md)")


def build_sdl(source: Path) -> None:
    """SDL3.dll, its import library, headers and licence into build/win/deps, built with llvm-mingw against the libusb
    package already unpacked there."""
    work = WIN / "sdl"
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    with tarfile.open(source) as t:
        t.extractall(work, filter="tar")
    # forward slashes: CMake reads a Windows path's backslashes as escapes (\U...) in its generated files
    deps, cc = WIN / "deps", (WIN / "llvm-mingw/bin/x86_64-w64-mingw32-").as_posix()
    exe = ".exe" if HOST_WINDOWS else ""
    src = work / f"SDL3-{SDL_VERSION}"
    configure = [
        "cmake", "-S", str(src), "-B", str(work / "build"), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_SYSTEM_NAME=Windows", "-DCMAKE_SYSTEM_PROCESSOR=x86_64", f"-DCMAKE_C_COMPILER={cc}clang{exe}",
        f"-DCMAKE_CXX_COMPILER={cc}clang++{exe}", f"-DCMAKE_RC_COMPILER={cc}windres{exe}", f"-DCMAKE_INSTALL_PREFIX={deps.as_posix()}",
        "-DSDL_STATIC=OFF", "-DSDL_TEST_LIBRARY=OFF", "-DSDL_TESTS=OFF", "-DSDL_EXAMPLES=OFF",
        "-DSDL_HIDAPI_LIBUSB=ON", "-DSDL_HIDAPI_LIBUSB_SHARED=ON",
        f"-DLibUSB_LIBRARY={(deps / 'lib/libusb-1.0.dll.a').as_posix()}", f"-DLibUSB_INCLUDE_PATH={(deps / 'include/libusb-1.0').as_posix()}",
    ]  # fmt: skip
    subprocess.run(configure, check=True, stdout=subprocess.DEVNULL)
    subprocess.run(["cmake", "--build", str(work / "build"), "--target", "install"], check=True,
                   stdout=subprocess.DEVNULL)  # fmt: skip
    config = (work / "build/include-config-release/build_config/SDL_build_config.h").read_text()
    if '#define SDL_LIBUSB_DYNAMIC "libusb-1.0.dll"' not in config:
        sys.exit("SDL3 built without libusb: the Switch 2 controllers would not start over USB")
    licence = deps / "share/licenses/SDL3"
    licence.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src / "LICENSE.txt", licence / "LICENSE.txt")
    shutil.rmtree(work)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--dry-run", action="store_true", help="list the pinned downloads and stop")
    args = ap.parse_args()
    if args.dry_run:
        for name, (url, want) in PINS.items():
            print(f"{name}: {url} {want}")
        return
    sdl_tools()
    DL.mkdir(parents=True, exist_ok=True)
    llvm, other = ("llvm-mingw-windows", "llvm-mingw") if HOST_WINDOWS else ("llvm-mingw", "llvm-mingw-windows")
    paths = {name: fetch(name) for name in PINS if name != other}
    unpack_llvm(paths.pop(llvm))
    vpx_license = paths.pop("libvpx-license")
    sdl_source = paths.pop("sdl3-src")
    unpack_deps(paths)
    build_sdl(sdl_source)
    (WIN / "deps/share/licenses/libvpx").mkdir(parents=True, exist_ok=True)
    shutil.copy2(vpx_license, WIN / "deps/share/licenses/libvpx/LICENSE")
    dlls = sorted(p.name for p in (WIN / "deps/bin").glob("*.dll"))
    print(f"build/win: llvm-mingw {LLVM_VERSION}, deps {', '.join(n for n in PINS if not n.startswith('llvm-mingw'))}; DLLs {' '.join(dlls)}")


if __name__ == "__main__":
    main()
