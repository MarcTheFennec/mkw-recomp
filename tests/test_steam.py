"""The Tools page's Add to Steam (tools/prepare/steam.py): the binary shortcuts.vdf read back as it was written, an
existing shortcut kept with its 64-bit field and tags, and an entry with the same executable replaced instead of
doubled. Seconds, on Linux and on Windows (ci.yml): an entry Steam reads and this script does not would drop the
player's own shortcuts from the file.

Usage: uv run pytest tests
"""

import os
import struct
import sys
import zlib
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools/prepare"))
import steam  # noqa: E402

NAME = "Mario Kart World"
EXE = "/games/mkw/mkw"


def vdf_with(rows: list[dict]) -> bytes:
    """The file steam.py writes: the shortcuts map, its rows by index."""
    return b"\x00shortcuts\x00" + steam.write_map({str(i): r for i, r in enumerate(rows)}) + b"\x08"


def row(appname: str, exe: str) -> dict:
    return {
        "appid": -1234567890, "AppName": appname, "Exe": f'"{exe}"', "StartDir": '"/games"', "icon": "",
        "ShortcutPath": "", "LaunchOptions": "", "IsHidden": 0, "AllowDesktopConfig": 1, "AllowOverlay": 1,
        "OpenVR": 0, "Devkit": 0, "DevkitGameID": "", "DevkitOverrideAppID": 0, "LastPlayTime": 1700000000,
        "FlatpakAppID": "", "tags": {"1": "played"},
    }  # fmt: skip


def test_write_reads_back(tmp_path: Path) -> None:
    # every type the format has: a string, two ints, a 64-bit one and a map of tags
    rows = [row("Older game", "/games/old/game")]
    rows[0]["u64field"] = ("u64", 18446744073709551615)
    p = tmp_path / "shortcuts.vdf"
    p.write_bytes(vdf_with(rows))
    got = steam.read_map(p.read_bytes(), 0)[0]["shortcuts"]
    assert [v for v in got.values()] == rows


def test_add_keeps_the_others_replaces_the_same_exe(tmp_path: Path) -> None:
    p = tmp_path / "shortcuts.vdf"
    others = [row("Older game", "/games/old/game"), row("Same exe", EXE)]
    others[0]["u64field"] = ("u64", 42)
    p.write_bytes(vdf_with(others))

    steam.add(p, NAME, EXE, "/games/mkw")
    got = list(steam.read_map(p.read_bytes(), 0)[0]["shortcuts"].values())

    names = [r["AppName"] for r in got]
    assert names == ["Older game", NAME]           # the same executable's row replaced, not doubled
    assert got[0]["u64field"] == ("u64", 42)        # an entry we do not write ourselves is kept whole
    assert got[0]["tags"] == {"1": "played"}
    want = zlib.crc32((f'"{EXE}"' + NAME).encode()) | 0x80000000
    assert struct.unpack("<i", struct.pack("<I", want))[0] == got[1]["appid"]   # Steam's own rule for a shortcut
    assert got[1]["Exe"] == f'"{EXE}"' and got[1]["StartDir"] == '"/games/mkw"'

    steam.add(p, NAME, EXE, "/games/mkw")          # a second add is not a second entry either
    assert len(steam.read_map(p.read_bytes(), 0)[0]["shortcuts"]) == 2


def test_add_creates_the_file(tmp_path: Path) -> None:
    p = tmp_path / "config" / "shortcuts.vdf"
    p.parent.mkdir()
    steam.add(p, NAME, EXE, "/games/mkw")
    got = list(steam.read_map(p.read_bytes(), 0)[0]["shortcuts"].values())
    assert [r["AppName"] for r in got] == [NAME]


def test_no_steam_pid_is_not_running(tmp_path: Path) -> None:
    # a pid file that is not there (or is not a number) reads as Steam closed, never as an error
    if os.name == "nt":
        pytest.skip("the Windows branch reads the registry instead")
    assert steam.steam_running(tmp_path / "steam") is False
    (tmp_path / "steam.pid").write_text("not a pid\n")
    assert steam.steam_running(tmp_path / "steam") is False
