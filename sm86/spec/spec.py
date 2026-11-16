"""Build the SM86 instruction spec the shader decoder reads (instructions.json) on this machine.

Why: the spec is NVIDIA's own description of the encoding, which nvdisasm only holds in memory: DocumentSASS catches it
by intercepting memcpy (intercept.c, funnel.py) and nvsass-disassembler parses it (parse_sidl.py, sidl_*.py), all MIT
and copied here. Being NVIDIA's text it is never versioned nor shipped: the player's setup (tools/prepare/prepare.py)
and a source checkout build it from CUDA 12.8's redistributable nvdisasm and ptxas, fetched from NVIDIA and pinned by
sha256. CUDA 12.8 is the last release whose nvdisasm still yields the text. nvdisasm needs any sm_86 cubin to run: one
is assembled from a minimal PTX with ptxas.

Linux: intercept.c as an LD_PRELOAD library. Windows: nvdisasm.exe links its C runtime statically, so intercept_win.c
is a DLL loaded into it while it is suspended, which rewrites the exe's own memcpy (inject below). Both give the same
spec for the decoder (09/11/2026: equal but for whitespace in the raw condition texts, which nothing reads).

Usage: python sm86/spec/spec.py [--out build/sm86] [--cache scratchpad/nvidia] [--cc "zig cc"] [--crt link.json]
       --crt (Windows, from the setup): the package's prebuilt C runtime, linked with zig lld-link, so zig does not
       build its libc on the player's PC (tools/prepare/machine.py lld_link; issue #2)
       (~1 min; downloads 84 MB on Linux, 126 MB on Windows, the first time)
"""

import argparse
import hashlib
import ssl
import json
import os
import re
import shutil
import subprocess
import sys
import platform
import tarfile
import tempfile
import urllib.request
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
WINDOWS = os.name == "nt"
REDIST = "https://developer.download.nvidia.com/compute/cuda/redist"
PINS = {  # tool: (package path under REDIST, sha256), from NVIDIA's redistrib_12.8.1.json
    "linux": {
        "nvdisasm": ("cuda_nvdisasm/linux-x86_64/cuda_nvdisasm-linux-x86_64-12.8.90-archive.tar.xz",
                     "8ac63f4079c47707ec958af3a97ceeb18194e16827a2766058f5872f6c3412ae"),
        "ptxas": ("cuda_nvcc/linux-x86_64/cuda_nvcc-linux-x86_64-12.8.93-archive.tar.xz",
                  "9961b3484b6b71314063709a4f9529654f96782ad39e72bf1e00f070db8210d3"),
    },
    "linux-aarch64": {  # NVIDIA's "sbsa" packages: the same tools for AArch64 Linux
        "nvdisasm": ("cuda_nvdisasm/linux-sbsa/cuda_nvdisasm-linux-sbsa-12.8.90-archive.tar.xz",
                     "250ae30e878b0c4c1553e7c7f28c2c763902c807b5f493ebc16e6e10c71213f2"),
        "ptxas": ("cuda_nvcc/linux-sbsa/cuda_nvcc-linux-sbsa-12.8.93-archive.tar.xz",
                  "dc0b713ce69fd921aa53ac68610717d126fc273a3c554b0465cf44d7e379f467"),
    },
    "windows": {
        "nvdisasm": ("cuda_nvdisasm/windows-x86_64/cuda_nvdisasm-windows-x86_64-12.8.90-archive.zip",
                     "a348209c803fa67b7bc216a5b4d7a6f9a117ccc60a8c567eb94cf25ba737ec47"),
        "ptxas": ("cuda_nvcc/windows-x86_64/cuda_nvcc-windows-x86_64-12.8.93-archive.zip",
                  "9fdc70b4271ed9aad4d64cd7076a7d96ec36512d074b9995fe638de669197391"),
    },
}  # fmt: skip
TRIES = 8  # rejected captures: 0 of 8 on Linux without ASLR, 3 of 8 on Windows (09/11/2026)
PTX = """.version 8.7
.target sm_86
.address_size 64
.visible .entry k(.param .u64 p)
{
  .reg .b64 %rd<2>;
  .reg .b32 %r<2>;
  ld.param.u64 %rd1, [p];
  mov.u32 %r1, %tid.x;
  st.global.u32 [%rd1], %r1;
  ret;
}
"""


FETCH_TRIES = 3


def https_context() -> ssl.SSLContext:
    """The CA file the setup chose (prepare.py exports it as SSL_CERT_FILE when the system's store is empty or
    unreadable to this Python), explicitly: on Windows the default context reads the system store whatever the variable
    says, and a certificate in it Python cannot parse stops every download (issue #456)."""
    ca = os.environ.get("SSL_CERT_FILE")
    return ssl.create_default_context(cafile=ca) if ca else ssl.create_default_context()


def download(url: str, part: Path) -> str:
    """The file at url into part; a short read or a broken connection is the reason, else ""."""
    try:
        with urllib.request.urlopen(url, timeout=60, context=https_context()) as r, open(part, "wb") as f:
            size = int(r.headers.get("Content-Length") or 0)
            shutil.copyfileobj(r, f)
    except OSError as e:
        return f"download stopped ({e})"
    got = part.stat().st_size
    return f"{got} of {size} bytes arrived" if size and got != size else ""


def fetch(rel: str, want: str, cache: Path) -> Path:
    """The pinned package, downloaded again when it arrives short or with another sha256: each try of #207 got a
    different checksum, which a connection cut or rewritten on the way gives and NVIDIA's file (checked) does not."""
    dst = cache / rel.rsplit("/", 1)[1]
    if dst.is_file() and hashlib.sha256(dst.read_bytes()).hexdigest() == want:
        return dst
    dst.unlink(missing_ok=True)
    part = dst.with_name(dst.name + ".part")
    why = ""
    for n in range(1, FETCH_TRIES + 1):
        why = download(f"{REDIST}/{rel}", part)
        if not why:
            got = hashlib.sha256(part.read_bytes()).hexdigest()
            if got == want:
                part.replace(dst)
                return dst
            why = f"{part.stat().st_size} bytes with sha256 {got}, expected {want}"
        print(f"{dst.name}: try {n} of {FETCH_TRIES}: {why}", file=sys.stderr)
    part.unlink(missing_ok=True)
    sys.exit(
        f"{dst.name}: the download from NVIDIA arrived damaged {FETCH_TRIES} times ({why}); an antivirus, proxy, "
        f"VPN or unstable connection on this PC changes or cuts it: pause them and run the setup again"
    )


def tool(name: str, cache: Path) -> Path:
    """bin/<name> of its pinned package, unpacked once into cache/bin."""
    exe = cache / "bin" / (name + ".exe" if WINDOWS else name)
    if exe.is_file():
        return exe
    host = "windows" if WINDOWS else "linux-aarch64" if platform.machine() in ("aarch64", "arm64") else "linux"
    rel, want = PINS[host][name]
    archive = fetch(rel, want, cache)
    exe.parent.mkdir(parents=True, exist_ok=True)
    if WINDOWS:
        with zipfile.ZipFile(archive) as z, open(exe, "wb") as dst:
            member = next(n for n in z.namelist() if n.endswith(f"/bin/{exe.name}"))
            with z.open(member) as src:
                shutil.copyfileobj(src, dst)
        return exe
    with tarfile.open(archive) as t:
        member = next(m for m in t.getmembers() if m.name.endswith(f"/bin/{name}"))
        with t.extractfile(member) as src, open(exe, "wb") as dst:
            shutil.copyfileobj(src, dst)
    exe.chmod(0o755)
    return exe


def strings(data: bytes) -> str:
    """`strings -n 1` of the capture. Windows' nvdisasm.exe copies the first piece of each text funnel.py looks for
    (ARCHITECTURE, OPERATION SETS) with blank lines in front, and funnel.py wants the word right after its
    "<dst src size>" mark: only those blank lines go. Elsewhere they are part of the text."""
    text = "\n".join(m.decode("ascii") for m in re.findall(rb"[\t\x20-\x7e]+", data)) + "\n"
    return re.sub(r"(<0x[0-9a-f]+ 0x[0-9a-f]+ \d+>\n)(?:[ \t]*\n)+(?=ARCHITECTURE|OPERATION SETS)", r"\1", text)


def no_aslr() -> None:
    """nvdisasm with fixed addresses (setarch -R): the captured buffers hold pointers, and with random ones the cut text
    came out garbled in 5 of ~17 tries, against 0 of 6 without (09/11/2026)."""
    import ctypes

    ctypes.CDLL(None).personality(0x0040000)  # ADDR_NO_RANDOMIZE


def intercept_lib(cc: list[str], work: Path, crt: Path | None) -> Path:
    if WINDOWS:
        dll = work / "intercept.dll"
        if not crt:
            subprocess.run([*cc, "-O2", "-shared", "-o", str(dll), str(HERE / "intercept_win.c")], check=True)
            return dll
        sys.path.insert(0, str(ROOT / "tools/prepare"))
        from machine import lld_link

        obj = work / "intercept_win.o"
        subprocess.run([*cc, "-O2", "-c", "-o", str(obj), str(HERE / "intercept_win.c")], check=True)
        subprocess.run(lld_link(cc[0], crt, dll, [str(obj)], f"-IMPLIB:{work / 'intercept.lib'}"), check=True)
        return dll
    so = work / "intercept.so"
    # -nostdlib and libc after the object: zig cc otherwise links its own memcpy and hides intercept.c's; -O2 and no
    # sanitizer: zig's debug default calls UBSan handlers nvdisasm does not have.
    subprocess.run([*cc, "-O2", "-fno-sanitize=undefined", "-fPIC", "-shared", "-nostdlib", "-o", str(so),
                    str(HERE / "intercept.c"), "-lc", "-ldl"], check=True)  # fmt: skip
    return so


def inject(proc: subprocess.Popen, dll: Path) -> None:
    """LoadLibraryW(dll) in the suspended process, from a thread of its own; then the process resumes."""
    import ctypes
    from ctypes import wintypes

    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    ntdll = ctypes.WinDLL("ntdll")
    k32.VirtualAllocEx.restype = ctypes.c_void_p
    k32.VirtualAllocEx.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_size_t, wintypes.DWORD, wintypes.DWORD]
    k32.WriteProcessMemory.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                       ctypes.c_void_p]  # fmt: skip
    k32.GetModuleHandleW.restype = wintypes.HMODULE
    k32.GetProcAddress.restype = ctypes.c_void_p
    k32.GetProcAddress.argtypes = [wintypes.HMODULE, ctypes.c_char_p]
    k32.CreateRemoteThread.restype = wintypes.HANDLE
    k32.CreateRemoteThread.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p,
                                       ctypes.c_void_p, wintypes.DWORD, ctypes.c_void_p]  # fmt: skip
    k32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    k32.GetExitCodeThread.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
    k32.CloseHandle.argtypes = [wintypes.HANDLE]
    ntdll.NtResumeProcess.argtypes = [wintypes.HANDLE]
    h = wintypes.HANDLE(int(proc._handle))
    path = ctypes.create_unicode_buffer(str(dll))
    mem = k32.VirtualAllocEx(h, None, ctypes.sizeof(path), 0x3000, 0x04)  # MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE
    if not mem or not k32.WriteProcessMemory(h, mem, path, ctypes.sizeof(path), None):
        raise OSError(f"cannot write the DLL path into nvdisasm.exe: {ctypes.get_last_error()}")
    load = k32.GetProcAddress(k32.GetModuleHandleW("kernel32.dll"), b"LoadLibraryW")
    thread = k32.CreateRemoteThread(h, None, 0, load, mem, 0, None)
    if not thread:
        raise OSError(f"cannot start a thread in nvdisasm.exe: {ctypes.get_last_error()}")
    k32.WaitForSingleObject(thread, 30000)
    module = wintypes.DWORD()
    k32.GetExitCodeThread(thread, ctypes.byref(module))
    k32.CloseHandle(thread)
    if not module.value:  # DllMain refused: the exe's memcpy is not where intercept_win.c expects it
        raise OSError("intercept.dll did not load into nvdisasm.exe")
    ntdll.NtResumeProcess(h)


def run_nvdisasm(nvdisasm: Path, lib: Path, cubin: Path) -> bytes:
    if not WINDOWS:
        env = dict(os.environ, LD_PRELOAD=str(lib), OMP_NUM_THREADS="1", OMP_THREAD_LIMIT="1")
        return subprocess.run([str(nvdisasm), str(cubin)], env=env, capture_output=True, check=True,
                              preexec_fn=no_aslr).stdout  # fmt: skip
    dump = cubin.with_suffix(".capture")
    with open(dump, "wb") as f:
        p = subprocess.Popen([str(nvdisasm), str(cubin)], stdout=f, stderr=subprocess.DEVNULL, creationflags=0x4)
        try:
            inject(p, lib)
        except OSError:
            p.kill()
            raise
        if p.wait():
            raise OSError(f"nvdisasm.exe exited with {p.returncode}")
    return dump.read_bytes()


def capture(nvdisasm: Path, lib: Path, work: Path, out: Path) -> str:
    """One interception of nvdisasm, cut and parsed into out/; the parser's complaint, or '' when the spec is good.

    It parses in work/spec and moves the result to out/ only when the parser accepts it: a rejected capture left a
    broken instructions.json in out/ that every later setup reused, since the shaders step only checks that it exists
    (issue #325)."""
    dump = run_nvdisasm(nvdisasm, lib, work / "sm_86.cubin")
    (work / "sm_86_intercept.txt").write_text(strings(dump), encoding="ascii")
    p = subprocess.run([sys.executable, str(HERE / "funnel.py"), str(work / "sm_86_intercept.txt")],
                       capture_output=True, text=True)  # fmt: skip
    if p.returncode:
        return f"funnel.py: {p.stderr[-2000:]}"
    stage = work / "spec"
    shutil.rmtree(stage, ignore_errors=True)
    stage.mkdir()
    for name in ("sm_86_instructions.txt", "sm_86_latencies.txt"):
        shutil.move(work / name, stage / name)
    # Windows' text sometimes puts two rows of a table on one line ("0 0 -> 0          0 1 -> 1"), and the parser then
    # orders the table's rows differently: one row per line, as in Linux's text (a no-op there).
    spec_txt = stage / "sm_86_instructions.txt"
    text = spec_txt.read_text(encoding="ascii")
    spec_txt.write_text(re.sub(r"(-> \S+)[ \t]{2,}(?=\d)", r"\1\n          ", text), encoding="ascii")
    # Exit 1 means "something was reported", not "no spec": the one known report (a missing separator in
    # f2f_f32_upconvert__RCR_RCR) is repaired by the parser itself, so anything beyond it fails.
    p = subprocess.run([sys.executable, str(HERE / "parse_sidl.py"), str(spec_txt), "-o", str(stage)],
                       capture_output=True, text=True)  # fmt: skip
    out.mkdir(parents=True, exist_ok=True)
    (out / "parse.log").write_text(p.stdout + p.stderr)  # the last try's, accepted or not, for a report
    if p.returncode > 1 or not re.search(r"^unparsed / suspect items: [01]$", p.stdout, re.M):
        return f"parse_sidl.py: {p.stdout[-2000:]}{p.stderr[-2000:]}"
    for f in stage.iterdir():
        (out / f.name).unlink(missing_ok=True)  # Windows' rename refuses an existing target
        shutil.move(f, out / f.name)
    return ""


def build(out: Path, cache: Path, cc: list[str], crt: Path | None) -> None:
    cache.mkdir(parents=True, exist_ok=True)
    nvdisasm, ptxas = tool("nvdisasm", cache), tool("ptxas", cache)
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        (work / "k.ptx").write_text(PTX)
        subprocess.run([str(ptxas), "-arch=sm_86", str(work / "k.ptx"), "-o", str(work / "sm_86.cubin")], check=True)
        lib = intercept_lib(cc, work, crt)
        # A garbled capture is still possible (see no_aslr; Windows keeps ASLR) and the parser has caught every one: a
        # few tries, ~3 s each.
        for attempt in range(1, TRIES + 1):
            err = capture(nvdisasm, lib, work, out)
            if not err:
                print(f"wrote {out / 'instructions.json'}")
                return
            print(f"capture {attempt} of {TRIES} rejected: {err.splitlines()[0]}", file=sys.stderr)
    sys.exit(err)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", type=Path, default=ROOT / "build/sm86")
    ap.add_argument("--cache", type=Path, default=ROOT / "scratchpad/nvidia", help="downloads and unpacked tools")
    ap.add_argument("--cc", default="cc", help='C compiler command, or a JSON list (["C:/a b/zig.exe", "cc"])')
    ap.add_argument("--crt", type=Path, help="Windows: the package's prebuilt C runtime (setup/lib/crt/link.json)")
    args = ap.parse_args()
    build(args.out, args.cache, cc_command(args.cc), args.crt)


def cc_command(arg: str) -> list[str]:
    """--cc: a JSON list from prepare.py, whose paths may have spaces ("C:\\Recomp C\\...", issue #29), or a plain
    command typed by hand."""
    return json.loads(arg) if arg.startswith("[") else arg.split()


if __name__ == "__main__":
    main()
