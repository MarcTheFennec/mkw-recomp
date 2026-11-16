"""The issue moderation's reading of a report (tools/github/classify.py) on the shapes players have sent: the bug form,
RecompLauncher's report without it, a bare title, and the replies that close an issue. A wrong reading closes a good
report, so each rule that closes or answers has its case here.

Usage: uv run pytest tests
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools/github"))

import classify  # noqa: E402

RELEASES = {f"v0.1.{i}" for i in range(8)}
SIGS = classify.load_signatures()

FORM = """### What happens

Crash at compiling game code, fatal error: 'gamelib.h' file not found

### Version

{ver}

### System

Windows 11 - GPU ( AMD 7800 XT ) - CPU - AMD ( 5700X3D )

### GPU

AMD (Radeon, Ryzen APU, Steam Deck, ROG Ally)

### Log

{log}"""

LAUNCHER = """**What happened:**

- Game: MKW Recomp v0.1.7
- System: Windows (x64)
- Started from RecompLauncher 0.90.0-beta.36
- Closed with code 70 after 79 s

From C:\\Users\\x\\AppData\\Roaming\\com.recomplauncher.app\\game-output\\mkw-recomp.log:
```
[host]   f3fcb854 d1fcc5cd
[host]   663ecddc de465b9e
[host]   656477ca 5c77383b
[host]   8b1f61a2 5c77383b
[host]   d604d639 5c77383b
FATAL: vk: upload submit failed (-4)
```"""

LOG = "[mkw.log](https://github.com/user-attachments/files/123/mkw.log)"


def test_version_from_form_launcher_and_prose():
    assert classify.version("crash", FORM.format(ver="v0.1.7", log=LOG), RELEASES) == "v0.1.7"
    assert classify.version("crash", FORM.format(ver="a1b2c3d", log=LOG), RELEASES) == "from-source"
    assert classify.version("Crash: closed with code 70", LAUNCHER, RELEASES) == "v0.1.7"
    # #58: the game's version in the field, ours in the prose
    body = FORM.format(ver="1.0.6", log="After compiling version 0.1.6 the game does not open")
    assert classify.version("It doesn't open", body, RELEASES) == "v0.1.6"
    assert classify.version("crash", "it crashes", RELEASES) is None


def test_gpu_ignores_the_cpu_vendor():
    assert classify.gpu("x", FORM.format(ver="v0.1.7", log="")) == "gpu-amd"
    assert classify.gpu("x", "### System\n\nWindows 11, Ryzen 5 7600 (AMD), RTX 4060") == "gpu-nvidia"
    assert classify.gpu("x", "### System\n\nRTX 4060 laptop with Radeon 780M") is None


def test_empty_and_game_requests():
    assert classify.is_empty(".", "s")
    assert classify.is_empty("crash", "### What happens\n\n_No response_\n\n### Log\n\n_No response_")
    assert not classify.is_empty("crash", LAUNCHER)
    assert classify.asks_for_game("where to find the xci", "")
    assert not classify.asks_for_game("how do I get the game to start", "it stays black after I download it")


def test_log_wanted_only_from_bug_reports():
    assert classify.has_log(LAUNCHER)
    assert classify.has_log(FORM.format(ver="v0.1.7", log=LOG))
    assert classify.wants_log("black screen", FORM.format(ver="v0.1.7", log="There are no logs"), {"bug"})
    assert not classify.wants_log("perf: stutter", "slow", set())
    assert not classify.wants_log("ultrawide", "please", {"enhancement"})
    # #257: a request without the form and without a failure in it
    assert not classify.wants_log("A community space outside GitHub", "Could you open a Matrix room?", set())
    assert classify.wants_log("Mkw.exe doesnt open", "I double click and nothing happens", set())
    assert classify.wants_log("x", "error code 71 when warming up shaders", set())


def test_known_errors_and_regressions():
    gamelib = FORM.format(ver="v0.1.1", log=LOG)
    assert classify.known_error(99, gamelib, "v0.1.1", SIGS).issue == 1
    # #84: the same text on a release after the fix is a regression, left open
    assert classify.known_error(84, gamelib, "v0.1.7", SIGS) is None
    assert classify.known_error(84, gamelib, None, SIGS) is None
    ipi = "vk: device fault: instruction pointer invalid\nFATAL: vk: vkQueueSubmit2 failed (-4)"
    assert classify.known_error(92, ipi, "v0.1.7", SIGS).issue == 22
    assert classify.known_error(22, ipi, "v0.1.7", SIGS) is None
    assert classify.known_error(94, LAUNCHER, "v0.1.7", SIGS) is None


def test_every_signature_links_its_wiki_section():
    # the bot's duplicate reply sends the player to what they can do (wiki Troubleshooting, one section per row)
    for s in SIGS:
        assert s.wiki and s.wiki == s.wiki.lower() and " " not in s.wiki, s


def test_still_happens_reopens():
    # #38: another player, a new log, under "fixed in v0.1.7"
    assert classify.still_happens(f"{LOG} Same error in v0.1.7 without GPL=0, freezes on title world map.", False)
    assert classify.still_happens("still freezes on the title", True)
    assert not classify.still_happens("still freezes for me too", False)
    assert not classify.still_happens("Thanks, it works now!", True)


def test_replies_that_settle():
    assert classify.reply_decision("Fixed in [v0.1.4](https://x/releases/tag/v0.1.4). On many PCs") == (
        "fixed",
        "v0.1.4",
    )
    assert classify.reply_decision("Thanks! This is the same as #36, fixed in v0.1.7") == ("fixed", "v0.1.7")
    assert classify.reply_decision("The missing flames are the same as #31, tracked there") == ("duplicate", 31)
    assert classify.reply_decision("Same signature as #22/#33/#34: the GPU faults") is None
    # #37: a reply that fixes part of another issue (#40) leaves this one open
    assert (
        classify.reply_decision("Same signature as #22.\n#40's second crash is a recompiler bug, fixed in v0.1.7.")
        is None
    )
    assert classify.reply_decision("Your log ends in `failed (-4)`: the GPU was lost, the same as #43 (RADV).") is None
    assert classify.reply_decision("Thanks for the log. The cause is the space.\nFixed in v0.1.5: https://x") == (
        "fixed",
        "v0.1.5",
    )
    assert classify.reply_decision("Please attach `logs/mkw.log`") is None


def test_modified_build_from_the_log_line():
    log = "[host] cfg: read mkw.ini\n[host] log: C:/mkw/logs/mkw.log (MKW Recomp 0.1.8-3-gabc1234-dirty)\n"
    assert classify.modified_build(log) == "0.1.8-3-gabc1234-dirty"
    assert classify.modified_build(log.replace("-dirty", "")) is None
    assert classify.modified_build("[host] log: C:/mkw/logs/mkw.log\n") is None   # up to 0.1.8: no version


def test_mods_from_the_log_line():
    log = "[host] cfg: read mkw.ini\n[host] mods: enabled ClassicGP, 30 FPS Lock\n[host] vk: gpu 0\n"
    assert classify.mods_enabled(log) == "ClassicGP, 30 FPS Lock"
    assert classify.mods_enabled("[host] cfg: read mkw.ini\n") is None


def test_behind_the_newest_two():
    rel = {"v0.1.8", "v0.1.9", "v0.1.10"}
    assert classify.behind("v0.1.8", rel)
    assert not classify.behind("v0.1.9", rel)
    assert not classify.behind("v0.1.10", rel)
    # labels that are not a published release never count as old
    assert not classify.behind("v0.1.11", rel)
    assert not classify.behind("unknown-version", rel)
    assert not classify.behind("from-source", rel)


def test_newest_log_and_the_setup_warm_up():
    warm = "[host] host save storage: D:\\mkw-recomp\\setup-work\\warm-profile/save/f755\n"
    ipi = "vk: device fault: instruction pointer invalid\n"
    # #425: the newest run is clean, an older mkw.N.log has #22's error: that error is not the report's
    newest, older = classify.split_logs([("mkw.log", "race ok\n"), ("mkw.4.log", ipi)], setup=False)
    assert [f[0] for f in newest] == ["mkw.log"] and [f[0] for f in older] == ["mkw.4.log"]
    # #437: only the warm-up's mkw.log attached to a race crash: no run of the player's
    newest, older = classify.split_logs([("mkw.log", warm)], setup=False)
    assert newest == [] and older == [("mkw.log", warm)]
    # a warm-up that crashed in the setup is the run the report describes
    assert classify.split_logs([("mkw.log", warm + ipi)], setup=True)[0] == [("mkw.log", warm + ipi)]
    # the warm-up's mkw.log skipped, the player's run in mkw.1.log is the newest
    newest, _ = classify.split_logs([("mkw.log", warm), ("mkw.1.log", ipi), ("mkw.2.log", "")], setup=False)
    assert newest == [("mkw.1.log", ipi)]
    # renamed copies ("teste.geral.mkw.log") count as mkw.log
    assert classify.log_age("teste.geral.mkw.log") == 0 and classify.log_age("mkw.3.log") == 3
