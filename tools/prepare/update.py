"""The start screen's update: the latest published release on GitHub, its package for this system downloaded and
checked, and the files the update left behind removed. Run with the package's Python by
runtime/ui/launcher/launcher_update.cpp; the launcher swaps the files itself once this has exited, then runs prepare.py
again without --source (only the steps whose key changed).

Why: players kept reporting from old versions (#28 from 0.1.2-12) and an update by hand means unpacking and preparing
again. The offer at start follows hookshot's host/hos_update.c (GPL-3.0, idea only): a few seconds at most, nothing
offline. Only published releases count (releases/latest: never drafts or prereleases).

Output: one event per line on stdout, as prepare.py's:
  check  "update <tag> <bytes>" when the latest release is newer than --version, "latest" when not
  fetch  "progress <done> <total>", then "ready <path>": the AppImage (Linux) or the package's top folder (Windows)
  clean  "cleaned <n>"
  any    "error <command> <message>"
Exit codes: 0 done, 1 failed.

Usage: python -I update.py check --version VER [--os linux|windows]
       python -I update.py fetch --tag TAG --dest DIR [--os linux|windows]
       python -I update.py clean --game DIR [--previous VERSION]
MKW_UPDATE_REPO=owner/name (default adriadam10/mkw-recomp) and MKW_UPDATE_AS=<version> (compare as that version: a
test) change what is asked.
"""

import argparse
import hashlib
import json
import os
import platform
import re
import shutil
import ssl
import sys
import time
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

REPO = os.environ.get("MKW_UPDATE_REPO") or "adriadam10/mkw-recomp"
CHECK_TIMEOUT = 5  # s: the start screen waits for nothing, but a hung request should not linger
FETCH_TIMEOUT = 30
ASSET_SUFFIX = {  # tools/build/release.py's names
    "linux": "-x86_64.AppImage",
    "linux-aarch64": "-aarch64.AppImage",
    "windows": "-windows-x64.zip",
}
CLEAN_WAIT = 20  # s
OLD_SUFFIX = ".old"  # what the launcher renames the replaced files to (Windows: a running exe cannot be removed)
PREVIOUS = "update-previous"  # the version an update replaced, kept for the Tools page's Undo (launcher_update.h)


def emit(*words: object) -> None:
    print(*words, flush=True)


def ca_file() -> str | None:
    """pip's bundled CA file when the system's store is empty to this Python, else None (python-build-standalone
    ships no certifi; a minimal Linux running the AppImage may have no /etc/ssl, and NixOS keeps its bundle where this
    OpenSSL does not look, issue #102; a Windows certificate store with a certificate Python cannot read raises
    OSError "invalid ASN1 tag" when the context is made, issue #456, and counts as empty)."""
    if os.environ.get("SSL_CERT_FILE"):
        return None
    try:
        if ssl.create_default_context().cert_store_stats().get("x509_ca"):
            return None
    except OSError:
        pass
    for pat in (
        "lib*/python3*/site-packages/pip/_vendor/certifi/cacert.pem",
        "Lib/site-packages/pip/_vendor/certifi/cacert.pem",
    ):
        for pem in Path(sys.prefix).glob(pat):
            return str(pem)
    return None


def ssl_context() -> ssl.SSLContext:
    """The system's CA store; else pip's bundled one (ca_file)."""
    return ssl.create_default_context(cafile=ca_file())


def get(url: str, timeout: int) -> urllib.request.addinfourl:
    req = urllib.request.Request(
        url, headers={"User-Agent": "mkw-recomp-launcher", "Accept": "application/vnd.github+json"}
    )
    return urllib.request.urlopen(req, timeout=timeout, context=ssl_context())


def version_key(v: str) -> tuple[int, int, int] | None:
    """(major, minor, patch) of a tag or of rt_port_version ("0.1.2-12-g1c15097" counts as 0.1.2); None for "dev"."""
    m = re.match(r"v?(\d+)\.(\d+)\.(\d+)", v)
    return (int(m[1]), int(m[2]), int(m[3])) if m else None


def latest_asset(os_name: str) -> tuple[str, dict]:
    with get(f"https://api.github.com/repos/{REPO}/releases/latest", CHECK_TIMEOUT) as r:
        rel = json.load(r)
    if rel.get("draft") or rel.get("prerelease"):
        raise ValueError("the latest release is not a published one")
    for a in rel.get("assets", []):
        if a["name"].endswith(ASSET_SUFFIX[os_name]):
            return rel["tag_name"], a
    raise ValueError(f"release {rel['tag_name']} has no package for {os_name}")


def check(args: argparse.Namespace) -> None:
    current = version_key(os.environ.get("MKW_UPDATE_AS") or args.version)
    tag, asset = latest_asset(args.os)
    latest = version_key(tag)
    if current is None or latest is None or latest <= current:
        emit("latest")
        return
    emit("update", tag, asset["size"])


def fetch(args: argparse.Namespace) -> None:
    tag, asset = latest_asset(args.os)
    if tag != args.tag:
        raise ValueError(f"the latest release is now {tag}, not {args.tag}")
    digest = asset.get("digest") or ""
    if not digest.startswith("sha256:"):
        raise ValueError(f"{asset['name']} has no sha256 on GitHub: not installed")
    dest = args.dest.resolve()
    dest.mkdir(parents=True, exist_ok=True)
    part = dest / (asset["name"] + ".part")
    h, done, total, step = hashlib.sha256(), 0, int(asset["size"]), max(1, int(asset["size"]) // 200)
    with get(asset["browser_download_url"], FETCH_TIMEOUT) as r, part.open("wb") as f:
        while chunk := r.read(1 << 20):
            f.write(chunk)
            h.update(chunk)
            if done // step != (done + len(chunk)) // step:
                emit("progress", done + len(chunk), total)
            done += len(chunk)
    if done != total or "sha256:" + h.hexdigest() != digest:
        part.unlink()
        raise ValueError(f"{asset['name']} came damaged ({done} of {total} bytes or another sha256): try again")
    if args.os == "linux":
        out = dest / asset["name"]
        part.replace(out)
        out.chmod(0o755)
        emit("ready", out)
        return
    new = dest / "new"
    shutil.rmtree(new, ignore_errors=True)
    with zipfile.ZipFile(part) as z:
        for name in z.namelist():  # nothing outside the folder, whatever the zip says
            if Path(name).is_absolute() or ".." in Path(name).parts:
                raise ValueError(f"{asset['name']} holds a path outside its folder: {name}")
        z.extractall(new)
    part.unlink()
    tops = [p for p in new.iterdir() if p.is_dir()]
    if len(tops) != 1 or not (tops[0] / "mkw.exe").is_file():
        raise ValueError(f"{asset['name']} is not a package of the port (no mkw-recomp/mkw.exe)")
    emit("ready", tops[0])


def clean(args: argparse.Namespace) -> None:
    """The files the update replaced: with --previous (the version they are), kept in PREVIOUS under their own names so
    Undo can put them back, the version before replaced; without, removed. The old program may still be closing when
    the new one starts this (Windows refuses to move or remove a running exe): each entry is tried again for CLEAN_WAIT
    seconds."""
    game, n = args.game.resolve(), 0
    olds = [p for p in game.iterdir() if p.name.endswith(OLD_SUFFIX)]
    keep = game / PREVIOUS
    if args.previous and olds:
        shutil.rmtree(keep, ignore_errors=True)
        keep.mkdir()
        (keep / "version").write_text(f"{args.previous}\n", encoding="utf-8")
    for p in olds:
        for _ in range(CLEAN_WAIT):
            try:
                if args.previous:
                    p.rename(keep / p.name.removesuffix(OLD_SUFFIX))
                else:
                    shutil.rmtree(p) if p.is_dir() and not p.is_symlink() else p.unlink()
                n += 1
                break
            except OSError:   # a running exe on Windows; in keep/, a leftover entry the previous clean-up left
                time.sleep(1)
    shutil.rmtree(game / "update", ignore_errors=True)
    emit("cleaned", n)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    # the package's own Python runs natively, so its machine is the package's: an AArch64 AppImage updates to the same
    host = "windows" if os.name == "nt" else "linux-aarch64" if platform.machine() in ("aarch64", "arm64") else "linux"
    c = sub.add_parser("check")
    c.add_argument("--version", required=True, help="this program's (rt_port_version)")
    c.add_argument("--os", choices=ASSET_SUFFIX, default=host)
    f = sub.add_parser("fetch")
    f.add_argument("--tag", required=True)
    f.add_argument("--dest", type=Path, required=True, help="where the download goes (next to what it replaces)")
    f.add_argument("--os", choices=ASSET_SUFFIX, default=host)
    k = sub.add_parser("clean")
    k.add_argument("--game", type=Path, required=True)
    k.add_argument("--previous", help="the version the replaced files are: kept for Undo instead of removed")
    args = ap.parse_args()
    try:
        {"check": check, "fetch": fetch, "clean": clean}[args.cmd](args)
    except (OSError, ValueError, KeyError, zipfile.BadZipFile, urllib.error.URLError) as e:
        emit("error", args.cmd, str(e).replace("\n", " "))
        sys.exit(1)


if __name__ == "__main__":
    main()
