"""Add the game to Steam as a non-Steam game, for Big Picture and the Steam Deck: an entry in every Steam user's
config/shortcuts.vdf (Steam's binary key-value format), the one Steam's own "Add a Non-Steam Game" writes. An entry with
the same executable is replaced, not doubled. Steam reads the file when it starts, so a running Steam shows the game
after a restart. The start screen's Tools page runs it (ui/launcher/launcher_tools.cpp).

The format, from Steam's files themselves: a map is 0x00 <name> NUL <entries> 0x08, a string 0x01 <name> NUL <value>
NUL, a 32-bit integer 0x02 <name> NUL <4 bytes LE>, a 64-bit one 0x07 <name> NUL <8 bytes LE>. The appid Steam derives
for a shortcut is crc32(exe + name) with the top bit set.

Prints one line for the start screen: "done <users> <running 0|1>", or "error <code> <detail>" (code: nosteam, io).

Usage: python tools/prepare/steam.py add --name "Mario Kart World" --exe <program> --dir <its folder>
"""

import argparse
import os
import struct
import sys
import zlib
from pathlib import Path


def steam_dir() -> Path | None:
    if os.name == "nt":
        import winreg

        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam") as k:
                return Path(winreg.QueryValueEx(k, "SteamPath")[0])
        except OSError:
            return None
    home = Path.home()
    for d in (
        home / ".steam/steam",
        home / ".local/share/Steam",
        home / ".var/app/com.valvesoftware.Steam/.local/share/Steam",
    ):
        if (d / "userdata").is_dir():
            return d
    return None


def steam_running(steam: Path) -> bool:
    if os.name == "nt":
        import winreg

        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam\ActiveProcess") as k:
                return bool(winreg.QueryValueEx(k, "pid")[0])
        except OSError:
            return False
    pid = steam.parent / "steam.pid" if steam.name == "steam" else Path.home() / ".steam/steam.pid"
    try:
        os.kill(int(pid.read_text().strip()), 0)
        return True
    except OSError, ValueError:
        return False


def read_map(b: bytes, i: int) -> tuple[dict, int]:
    out: dict = {}
    while b[i] != 0x08:
        t, i = b[i], i + 1
        end = b.index(0, i)
        key, i = b[i:end].decode("utf-8", "replace"), end + 1
        if t == 0x00:
            out[key], i = read_map(b, i)
        elif t == 0x01:
            end = b.index(0, i)
            out[key], i = b[i:end].decode("utf-8", "replace"), end + 1
        elif t == 0x02:
            out[key], i = struct.unpack_from("<i", b, i)[0], i + 4
        elif t == 0x07:
            out[key], i = ("u64", struct.unpack_from("<Q", b, i)[0]), i + 8
        else:
            raise ValueError(f"unknown type {t} at {i}")
    return out, i + 1


def write_map(m: dict) -> bytes:
    out = bytearray()
    for k, v in m.items():
        name = k.encode() + b"\0"
        if isinstance(v, dict):
            out += b"\x00" + name + write_map(v)
        elif isinstance(v, tuple):
            out += b"\x07" + name + struct.pack("<Q", v[1])
        elif isinstance(v, int):
            out += b"\x02" + name + struct.pack("<i", v)
        else:
            out += b"\x01" + name + str(v).encode() + b"\0"
    return bytes(out + b"\x08")


def add(vdf: Path, name: str, exe: str, start_dir: str) -> None:
    root = read_map(vdf.read_bytes(), 0)[0] if vdf.is_file() and vdf.stat().st_size else {}
    rows = list(root.get("shortcuts", {}).values())
    quoted = f'"{exe}"'
    rows = [r for r in rows if r.get("Exe", r.get("exe")) != quoted]  # replaced, not doubled
    appid = zlib.crc32((quoted + name).encode()) | 0x80000000
    rows.append({
        "appid": struct.unpack("<i", struct.pack("<I", appid))[0], "AppName": name, "Exe": quoted,
        "StartDir": f'"{start_dir}"', "icon": exe, "ShortcutPath": "", "LaunchOptions": "", "IsHidden": 0,
        "AllowDesktopConfig": 1, "AllowOverlay": 1, "OpenVR": 0, "Devkit": 0, "DevkitGameID": "",
        "DevkitOverrideAppID": 0, "LastPlayTime": 0, "FlatpakAppID": "", "tags": {},
    })  # fmt: skip
    data = b"\x00shortcuts\x00" + write_map({str(i): r for i, r in enumerate(rows)}) + b"\x08"
    tmp = vdf.with_suffix(".vdf.part")
    tmp.write_bytes(data)
    tmp.replace(vdf)  # never a half-written file Steam would drop every shortcut from


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("add")
    a.add_argument("--name", required=True)
    a.add_argument("--exe", required=True)
    a.add_argument("--dir", required=True)
    args = ap.parse_args()
    steam = steam_dir()
    users = [u for u in (steam / "userdata").iterdir() if u.name.isdigit() and u.name != "0"] if steam else []
    if not users:
        print("error nosteam", flush=True)
        sys.exit(1)
    try:
        for u in users:
            (u / "config").mkdir(exist_ok=True)
            add(u / "config/shortcuts.vdf", args.name, args.exe, args.dir)
    except (OSError, ValueError) as e:
        print("error io", e, flush=True)
        sys.exit(1)
    print("done", len(users), int(steam_running(steam)), flush=True)


if __name__ == "__main__":
    main()
