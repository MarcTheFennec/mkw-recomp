"""Reads an issue's text without GitHub: which release it reports, its GPU, whether it is empty, asks for a pirated game,
lacks a log or matches a known error (signatures.toml), and what a maintainer's reply decides. moderate.py acts on it;
tests/test_issues.py checks it on the shapes players have sent (the bug form, RecompLauncher's report, a bare title).
"""

import re
import tomllib
from dataclasses import dataclass
from pathlib import Path

SIGNATURES = Path(__file__).with_name("signatures.toml")

FENCE = re.compile(r"```.*?(?:```|\Z)", re.S)
FILE_ATTACHMENT = re.compile(r"https://github\.com/user-attachments/files/\d+/")
ANY_ATTACHMENT = re.compile(r"https://github\.com/user-attachments/")
SEMVER = re.compile(r"\bv?(\d+\.\d+\.\d+)\b")
COMMIT = re.compile(r"^[0-9a-f]{7,40}$")
# RecompLauncher (github.com/elripalda/RecompLauncher) files its own report: "- Game: MKW Recomp v0.1.7" and the tail of
# the game's log in a fence, without our form.
LAUNCHER_VERSION = re.compile(r"Game:\s*MKW Recomp v?(\d+\.\d+\.\d+)", re.I)
# Only words no player uses for a crash: "how do I get the game to start" must not close an issue
PIRACY = re.compile(r"\b(xci|nsp|nsz|prod\.keys|title\.keys)\b|\b(rom download|download (the |a )?rom)\b", re.I)
# Form answers that carry no information of their own
FORM_FILLER = re.compile(r"^###.*$|_No response_", re.M)
# Without the bug form, only text that describes a failure asks for a log (#257, a request for a Discord, got "please
# attach mkw.log"): "doesn't open" and "code 71" count, "could you open a Matrix room" does not.
BUG_FORM = re.compile(r"^### What happens\s*$", re.M)
FAILURE = re.compile(
    r"\b(crash\w*|error|fail\w*|freez\w*|hang\w*|stuck|glitch\w*|artifact\w*|black screen|bug|broken|closes?|"
    r"code \d+|does nothing|nothing happens|(?:doesn'?t|does not|won'?t|can'?t|cannot|not) (?:open|work|start|launch|load|run))\b",
    re.I,
)

GPU_PATTERNS = {
    "gpu-nvidia": re.compile(r"\b(nvidia|geforce|rtx\s?\d|gtx\s?\d)", re.I),
    # Not a bare "AMD": the CPU line ("AMD Ryzen 7 5700X3D") would label every Ryzen owner
    "gpu-amd": re.compile(r"\b(radeon|radv|rx\s?\d{3,4}\b|\d{3,4}\s?xtx?\b|steam deck|rog ally)", re.I),
    "gpu-intel": re.compile(r"\b(intel\s+arc|arc\s+[ab]\d{3}|iris|uhd graphics|\banv\b)", re.I),
}

# A reply settles the issue only when the fix or the duplicate is its subject: "Fixed in v0.1.4." opening a sentence,
# "this is the same as #36, fixed in v0.1.7". "#40's second crash is a recompiler bug, fixed in v0.1.7" in a reply to
# #37 fixes part of another issue, and ", the same as #43" next to a request for a log only groups it.
FIXED_IN = re.compile(r"(?:^|[.!:]\s+)Fixed in \[?v(\d+\.\d+\.\d+)|\bsame as #\d+, fixed in \[?v(\d+\.\d+\.\d+)", re.I)
SAME_AS = re.compile(r"\b(?:is|are) the same as #(\d+)", re.I)


def vkey(tag: str) -> tuple[int, ...]:
    return tuple(int(p) for p in tag.lstrip("v").split("."))


def behind(label: str, releases: set[str], keep: int = 2) -> bool:
    """A release label (`v0.1.7`) older than the newest `keep` published releases: a report there is asked to try the
    latest before it is worked on (14/11/2026: 43 of 130 open issues were on 0.1.8 or older, untouched for days)."""
    if not re.fullmatch(r"v\d+\.\d+\.\d+", label) or label not in releases:
        return False
    newest = sorted(releases, key=vkey, reverse=True)[:keep]
    return vkey(label) < vkey(newest[-1])


def form_field(body: str, label: str) -> str | None:
    m = re.search(rf"^### {re.escape(label)}\s*\n+(.*?)(?=^### |\Z)", body, re.M | re.S)
    return m.group(1).strip() if m else None


def version(title: str, body: str, releases: set[str]) -> str | None:
    """The release label (`v0.1.7`), `from-source` for a commit, or None. Only published releases count: a player's
    "1.0.6" is the game's version misread, so the text around it is read next."""
    field = form_field(body, "Version")
    if field:
        if COMMIT.match(field.split()[0].lower()):
            return "from-source"
        for v in SEMVER.findall(field):
            if f"v{v}" in releases:
                return f"v{v}"
    m = LAUNCHER_VERSION.search(body)
    if m and f"v{m.group(1)}" in releases:
        return f"v{m.group(1)}"
    for text in (title, body):
        for v in SEMVER.findall(text):
            if f"v{v}" in releases:
                return f"v{v}"
    return None


def gpu(title: str, body: str) -> str | None:
    """The GPU label when exactly one vendor is named (a laptop naming both is left to a person)."""
    text = f"{title}\n{form_field(body, 'System') or body}"
    found = [label for label, pat in GPU_PATTERNS.items() if pat.search(text)]
    return found[0] if len(found) == 1 else None


def is_empty(title: str, body: str) -> bool:
    """Nothing to act on: under 30 characters once the form's headings and blank answers are gone, and no attachment."""
    text = FORM_FILLER.sub("", body or "")
    return len("".join(text.split())) < 30 and not ANY_ATTACHMENT.search(text)


def asks_for_game(title: str, body: str) -> bool:
    """Asks where to get the game or its keys (closed by the form's own rule). Logs are left out: their paths and
    hashes are not the player's words."""
    return bool(PIRACY.search(f"{title}\n{FENCE.sub('', body or '')}"))


def has_log(body: str) -> bool:
    """A log file attached, or pasted as a fence of at least five lines (RecompLauncher pastes its tail)."""
    if FILE_ATTACHMENT.search(body or ""):
        return True
    return any(block.count("\n") >= 6 for block in FENCE.findall(body or ""))


def wants_log(title: str, body: str, labels: set[str]) -> bool:
    """A bug report needs its log; a performance report or a feature request does not."""
    if labels & {"performance", "enhancement"} or title.lower().startswith(("perf:", "[feature")):
        return False
    if not BUG_FORM.search(body) and not FAILURE.search(f"{title}\n{FENCE.sub('', body)}"):
        return False
    return not has_log(body)


@dataclass(frozen=True)
class Signature:
    issue: int
    all: tuple[str, ...]
    what: str
    fixed: str | None = None
    wiki: str = ""  # anchor of its section in the wiki's Troubleshooting page


def load_signatures(path: Path = SIGNATURES) -> list[Signature]:
    rows = tomllib.loads(path.read_text())["signature"]
    return [
        Signature(r["issue"], tuple(s.lower() for s in r["all"]), r["what"], r.get("fixed"), r.get("wiki", ""))
        for r in rows
    ]


def known_error(number: int, text: str, ver: str | None, sigs: list[Signature]) -> Signature | None:
    """The first signature whose strings are all in the text. A fixed one needs the report's release to be older than
    the fix: on the fix's release or later, or with no release, it may be a regression and is left to a person."""
    low = text.lower()
    for s in sigs:
        if s.issue == number or not all(part in low for part in s.all):
            continue
        if s.fixed and (ver is None or not ver.startswith("v") or vkey(ver) >= vkey(s.fixed)):
            continue
        return s
    return None


# A log's place in the rotation from its attachment's name: mkw.log 0, mkw.1.log 1... (a renamed copy, "x.mkw.log", 0)
ROTATED = re.compile(r"mkw\.(\d+)\.log$", re.I)
# The setup's warm-up run (tools/prepare/prepare.py warm): its saves go to setup-work/warm-profile. Before 0.1.13 it
# wrote mkw.log, pushing the player's run to mkw.1.log (#437, 16/11/2026: the attached mkw.log was the warm-up's)
WARM_UP = re.compile(r"setup-work[\\/]+warm-profile", re.I)


def is_warm_up(log: str) -> bool:
    return bool(WARM_UP.search(log))


def log_age(name: str) -> int:
    return int(m.group(1)) if (m := ROTATED.search(name)) else 0


def split_logs(files: list[tuple[str, str]], setup: bool) -> tuple[list[tuple[str, str]], list[tuple[str, str]]]:
    """The attached logs (name, text) as (newest, older): newest is the most recent game run among them, the one a
    report describes; a warm-up counts as older unless the report is about the setup. A known error only in an older
    log may be another run's (#425, 15/11/2026: an ultrawide report closed as #22 for its mkw.N.log)."""
    runs = [f for f in files if setup or not is_warm_up(f[1])]
    if not runs:
        return [], files
    age = min(log_age(f[0]) for f in runs)
    newest = [f for f in runs if log_age(f[0]) == age]
    return newest, [f for f in files if f not in newest]


STILL = re.compile(
    r"\b(still|again|persists?|same (thing|error|issue|problem|crash|freeze)|not (fixed|working)|doesn'?t work)\b", re.I
)


PORT_BUILD = re.compile(r"\[host\] log: .*\(MKW Recomp ([^)\s]+)\)")


def modified_build(log: str) -> str | None:
    """The port version of a log from a build with uncommitted changes (git describe's "-dirty", which the runtime
    writes on the log's first line after 0.1.8), else None. #88 (11/11/2026): a flicker video from a build with its own
    ASTC transcoder, found only by a log line that is in no commit."""
    m = PORT_BUILD.search(log or "")
    return m.group(1) if m and m.group(1).endswith("-dirty") else None


MODS_ON = re.compile(r"\[host\] mods: enabled (.+)")


def mods_enabled(log: str) -> str | None:
    """The mods a log's run had on (the runtime's "mods: enabled A, B" line, from 0.1.10), else None: a mod's bug
    is its author's, so the bot asks for a run without them first."""
    m = MODS_ON.search(log or "")
    return m.group(1).strip() if m else None


def still_happens(body: str, by_author: bool) -> bool:
    """A comment on a closed issue saying the fix did not work: from its author, or from anyone with a new log."""
    return bool(STILL.search(body or "")) and (by_author or has_log(body))


def reply_decision(body: str) -> tuple[str, str | int] | None:
    """What a maintainer's reply decides: ("fixed", "v0.1.7") or ("duplicate", 36). "Same signature as #22" is not a
    duplicate: it groups a crash with others while asking for more."""
    m = FIXED_IN.search(body)
    if m:
        return "fixed", f"v{m.group(1) or m.group(2)}"
    m = SAME_AS.search(body)
    if m:
        return "duplicate", int(m.group(1))
    return None
