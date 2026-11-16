# /// script
# requires-python = ">=3.12"
# ///
"""Moderates the GitHub issues: labels each with the release it reports and its GPU, closes the empty and the ones asking
for the game, asks for the log, answers known errors (signatures.toml) and closes what a maintainer's reply settles.

Why: by 11/11/2026, 85 issues in two days; replies saying "Fixed in v0.1.4" or "same as #36" left them open, the same
crashes came in several issues, and RecompLauncher's reports (no form) got no labels. It runs from
.github/workflows/issues.yml on each event and once a day; by hand, it prints what it would do unless --apply.

Rules (the bot never comments where a maintainer already did):
- empty (under 30 characters, no attachment) or asking for the game: closed as not planned
- a bug report without a log and no replies yet: `needs-logs`, closed as not planned after 7 days without a reply from its author
- a known error: `duplicate` and "looks like #N", closed as its duplicate at once; its author's reply reopens it
  (14/11/2026: the 3 days, swept once a day and late, left 13 labelled duplicates open for up to 5 days)
- a report on a release older than the newest two, quiet for 3 days: `needs-retry`, asked to try the latest; closed as
  not planned after 7 days without a reply from its author (14/11/2026: 43 of 130 open issues were on 0.1.8 or older)
- a maintainer's reply with "Fixed in vX": closed if vX is out, else `fixed-next-release`; with "same as #N": duplicate
- a closed issue is reopened when its author, or anyone attaching a log, says it still happens
- a published release closes the `fixed-next-release` issues with a link to it
- a log from a run with mods on: a note asking for the same run without them (mods are their authors'); label `mods`
- a known error only in an older log than the newest run's (mkw.N.log): a note pointing to its issue, left open
- only the setup's warm-up log attached to a report not about the setup: a note asking for the run's own log

Usage: uv run tools/github/moderate.py issue N | comment N COMMENT_ID | sweep | release TAG | backlog [--apply]
"""

import argparse
import json
import os
import re
import subprocess
import urllib.request
from datetime import UTC, datetime, timedelta

import classify

MAINTAINERS = {"OWNER", "MEMBER", "COLLABORATOR"}
MARK = "<!-- mkw-bot:{} -->"
DUP_DAYS, LOG_DAYS = 0, 7  # DUP_DAYS 0: duplicates labelled before 14/11/2026, when they waited 3 days, close now
QUIET_DAYS, RETRY_DAYS = 3, 7
NOT_RETRIED = {"duplicate", "needs-logs", "needs-retry", "fixed-next-release", "enhancement"}
NEW_LABELS = {
    "performance": ("fbca04", "Performance report: frame times on a player's machine"),
    "from-source": ("ededed", "Reported on a build from source"),
    "unknown-version": ("ededed", "The release could not be read from the report"),
    "needs-retry": ("d4c5f9", "Reported on an older release: waiting for a try on the latest"),
}

TEXT = {
    "wiki": " What you can do about it is in the wiki: [Troubleshooting]({repo_url}/wiki/Troubleshooting#{wiki}).",
    "empty": "This issue has no details to act on, so it is closed. Please open a new one with the "
    "[bug report form]({repo_url}/issues/new?template=bug_report.yml) and attach `logs/mkw.log`.",
    "game": "Only Mario Kart World 1.0.0 or 1.8.0 dumped from your own console is supported; issues about obtaining the game, "
    "its files or keys are closed.",
    "logs": "Thanks for the report. To find the cause we need the log: please attach `logs/mkw.log` from the game "
    "folder (or `logs/setup.log` and `logs/warm.log` if the setup failed) by dragging it into a comment; the wiki says "
    "[where it is]({repo_url}/wiki/Installation#where-things-live). Without it this issue is closed "
    f"in {LOG_DAYS} days.",
    "dup": "Thanks for the report. This looks like #{issue}: {what}. Please follow and add to that issue instead; this "
    "one is closed as its duplicate. If yours is a different problem, reply here and it is reopened.",
    "dup_old": "Thanks for the report. This looks like #{issue}: {what}, fixed in {fixed}. You are on {ver}: please "
    "update to the [latest release]({repo_url}/releases/latest). This one is closed as its duplicate; if it still "
    "happens there, reply here with its `logs/mkw.log` and it is reopened.",
    "dup_next": "Thanks for the report. This looks like #{issue}: {what}. It is fixed in `main` and comes in {fixed}, "
    "which is not out yet: please follow #{issue}, it is closed with a link when {fixed} is published. This one is "
    "closed as its duplicate; if it still happens on {fixed}, reply here with its `logs/mkw.log` and it is reopened.",
    "retry": "This was reported on {ver}, and {latest} is out with many fixes since. Please try the "
    "[latest release]({repo_url}/releases/latest) and reply here: whether it still happens, with its `logs/mkw.log` "
    f"attached. Without a reply this issue is closed in {RETRY_DAYS} days.",
    "retry_closed": f"Closed: no reply in {RETRY_DAYS} days. If it still happens on the latest release, reply here with "
    "`logs/mkw.log` attached and it is reopened, or open a new issue.",
    "logs_closed": f"Closed: no log in {LOG_DAYS} days. If it still happens on the latest release, open a new issue "
    "with `logs/mkw.log` attached.",
    "dup_closed": "Closed as a duplicate of #{issue}.",
    "modified": "Note: this log is from a build with local changes (`MKW Recomp {ver}`). Please check whether it "
    "happens with a [release package]({repo_url}/releases/latest) or a clean build of `main`: a change outside the "
    "repository can cause it, and we cannot see it.",
    "mods": "Note: this run had mods on ({mods}). Mods are made by other people and can cause crashes and glitches "
    "the game does not have: please try again with them off (Mods page, Installed) and attach that `logs/mkw.log`. "
    "If it only happens with a mod, please report it to the mod's author.",
    "release": "Fixed in [{tag}]({url}). If it still happens there, open a new issue with `logs/mkw.log` attached.",
    "dup_older": "Note: an older log here (`{name}`) has the error of #{issue}: {what}. If that is the problem you "
    "report, please follow #{issue}; the newest log doesn't show it, so this issue stays open.",
    "warm": "Note: the attached log is from the setup's warm-up (a short start of the game while it sets up; its saves "
    "are in `setup-work/warm-profile`), not from the run you describe. Each start moves the log before it to "
    "`mkw.1.log`, `mkw.2.log`…: please attach the one from the run with the problem (its first lines name the "
    "version). From 0.1.13 the warm-up writes its own `logs/warm.log`.",
}


LOG_FILE = re.compile(r"https://github\.com/user-attachments/files/\d+/[^\s)\]\"']+\.(?:log|txt)")


def attached_logs(text: str) -> list[tuple[str, str]]:
    """(file name, text) of the .log/.txt files attached in text (the first 4, up to 4 MiB each). A known error is
    usually only in the log: #96 (11/11/2026) had "instruction pointer invalid" in mkw.1.log and nothing in its body."""
    out = []
    for url in list(dict.fromkeys(LOG_FILE.findall(text)))[:4]:
        try:
            with urllib.request.urlopen(url, timeout=30) as r:
                out.append((url.rsplit("/", 1)[-1], r.read(4 << 20).decode("utf-8", "replace")))
        except OSError as e:
            print(f"  {url}: {e}")
    return out


class GitHub:
    """gh with GH_REPO set; writes are printed instead of run unless apply."""

    def __init__(self, repo: str, apply: bool) -> None:
        self.repo, self.apply = repo, apply
        self.env = {**os.environ, "GH_REPO": repo}

    def get(self, path: str) -> object:
        out = subprocess.run(
            ["gh", "api", "--paginate", "--slurp", f"repos/{self.repo}/{path}"],
            capture_output=True,
            text=True,
            check=True,
            env=self.env,
        ).stdout
        pages = json.loads(out)
        return [x for p in pages for x in p] if pages and isinstance(pages[0], list) else pages[0]

    def run(self, *args: str) -> None:
        print("  " + " ".join(a if len(a) < 60 else a[:57] + "..." for a in args))
        if self.apply:
            subprocess.run(["gh", *args], check=True, env=self.env, stdout=subprocess.DEVNULL)

    def label(self, n: int, *labels: str) -> None:
        self.run("issue", "edit", str(n), *(f"--add-label={lb}" for lb in labels))

    def unlabel(self, n: int, *labels: str) -> None:
        self.run("issue", "edit", str(n), *(f"--remove-label={lb}" for lb in labels))

    def comment(self, n: int, kind: str, text: str) -> None:
        self.run("issue", "comment", str(n), "--body", f"{text}\n\n{MARK.format(kind)}")

    def close(self, n: int, reason: str, text: str | None = None, dup_of: int | None = None) -> None:
        args = ["issue", "close", str(n)]
        args += [f"--duplicate-of={dup_of}"] if dup_of else [f"--reason={reason}"]
        self.run(*args, *(["--comment", text] if text else []))


class Moderator:
    def __init__(self, gh: GitHub) -> None:
        self.gh = gh
        self.repo_url = f"https://github.com/{gh.repo}"
        rel = gh.get("releases?per_page=100")
        self.releases = {r["tag_name"]: r["html_url"] for r in rel if not r["draft"]}
        self.sigs = classify.load_signatures()
        self.labels = {lb["name"] for lb in gh.get("labels?per_page=100")}

    def ensure_label(self, name: str) -> None:
        if name in self.labels:
            return
        color, desc = NEW_LABELS.get(name, ("c2e0c6", f"Reported on {name}"))
        self.gh.run("label", "create", name, "--color", color, "--description", desc)
        self.labels.add(name)

    def comments(self, n: int) -> list[dict]:
        return self.gh.get(f"issues/{n}/comments?per_page=100")

    def on_issue(self, issue: dict, comments: list[dict]) -> None:
        """Labels and first answer of a new or edited issue."""
        n, title, body = issue["number"], issue["title"], issue["body"] or ""
        if issue["state"] != "open" or issue["author_association"] in MAINTAINERS:
            return
        have = {lb["name"] for lb in issue["labels"]}
        print(f"#{n} {title[:70]}")
        human = any(c["author_association"] in MAINTAINERS for c in comments)
        said = "\n".join(c["body"] for c in comments)
        # The log often comes in a later comment of its author
        reported = "\n".join([body, *(c["body"] for c in comments if c["user"]["login"] == issue["user"]["login"])])
        if classify.is_empty(title, body) and not human:
            self.gh.label(n, "invalid")
            self.gh.close(n, "not planned", TEXT["empty"].format(repo_url=self.repo_url))
            return
        if classify.asks_for_game(title, body):
            self.gh.label(n, "invalid")
            self.gh.close(n, "not planned", TEXT["game"])
            return
        ver = classify.version(title, body, set(self.releases)) or "unknown-version"
        add = {ver} - have
        stale = {lb for lb in have if (lb[:1] == "v" and lb[1:2].isdigit()) or lb in NEW_LABELS} - {ver, "performance"}
        if not have & set(classify.GPU_PATTERNS) and (g := classify.gpu(title, body)):
            add.add(g)
        if title.lower().startswith("perf:"):
            add.add("performance")
        for lb in add:
            self.ensure_label(lb)
        if add:
            self.gh.label(n, *sorted(add))
        if stale:
            self.gh.unlabel(n, *sorted(stale))
        if human:
            return
        files = attached_logs(reported)
        logs = "\n".join(t for _, t in files)
        setup = (classify.form_field(body, "Where") or "").startswith("Start screen or setup")
        newest, older = classify.split_logs(files, setup)
        if (mod := classify.modified_build(logs)) and MARK.format("modified") not in said:
            self.gh.comment(n, "modified", TEXT["modified"].format(ver=mod, repo_url=self.repo_url))
        if on := classify.mods_enabled(logs):
            if "mods" not in have:
                self.gh.label(n, "mods")  # made on GitHub by hand (#274), not in NEW_LABELS: those are swept as stale
            if MARK.format("mods") not in said:
                self.gh.comment(n, "mods", TEXT["mods"].format(mods=on))
        sig = classify.known_error(n, "\n".join([title, body, *(t for _, t in newest)]), ver, self.sigs)
        for name, text in [] if sig else older:
            if (s := classify.known_error(n, text, ver, self.sigs)) and MARK.format("dup_older") not in said:
                self.gh.comment(n, "dup_older", TEXT["dup_older"].format(name=name, issue=s.issue, what=s.what))
                break
        if files and not newest and not sig and MARK.format("warm") not in said:
            self.gh.comment(n, "warm", TEXT["warm"])
        if sig and MARK.format("dup") not in said:
            # #483 (16/11/2026): "fixed in v0.1.13, please update" while v0.1.12 was the newest release
            key = ("dup_old" if sig.fixed in self.releases else "dup_next") if sig.fixed else "dup"
            text = TEXT[key].format(issue=sig.issue, what=sig.what, fixed=sig.fixed, ver=ver, repo_url=self.repo_url)
            if sig.wiki:
                text += TEXT["wiki"].format(repo_url=self.repo_url, wiki=sig.wiki)
            self.gh.label(n, "duplicate")
            self.gh.comment(n, "dup", text)
            self.gh.close(n, "duplicate", dup_of=sig.issue)
        elif "needs-logs" in have and classify.has_log(reported):
            self.gh.unlabel(n, "needs-logs")
        elif not sig and not comments and classify.wants_log(title, reported, have | add):
            self.gh.label(n, "needs-logs")
            self.gh.comment(n, "logs", TEXT["logs"].format(repo_url=self.repo_url))

    def on_comment(self, issue: dict, c: dict) -> bool:
        """A maintainer's reply that settles the issue, or its author answering the bot. True if it closed it."""
        n = issue["number"]
        have = {lb["name"] for lb in issue["labels"]}
        if issue["state"] != "open":
            # #38 (11/11/2026): closed by a "fixed in v0.1.7" reply, then "same error in v0.1.7" with a new log
            mine = c["user"]["login"] == issue["user"]["login"]
            # closed by the bot as a known error or for no reply: whatever its author says next reopens it
            by_bot = (
                mine
                and ({"duplicate", "needs-retry"} & have)
                and any(MARK.format(k) in x["body"] for x in self.comments(n) for k in ("dup", "retry"))
            )
            if by_bot:
                print(f"#{n} {issue['title'][:70]}: its author replied after the bot closed it, reopened")
                self.gh.run("issue", "reopen", str(n))
                self.gh.unlabel(n, *sorted({"duplicate", "needs-retry"} & have))
                return False
            if c["author_association"] not in MAINTAINERS and classify.still_happens(c["body"], mine):
                print(f"#{n} {issue['title'][:70]}: still happens, reopened")
                self.gh.run("issue", "reopen", str(n))
                if "fixed-next-release" in have:
                    self.gh.unlabel(n, "fixed-next-release")
            return False
        if c["author_association"] in MAINTAINERS:
            decision = classify.reply_decision(c["body"])
            if not decision:
                return False
            print(f"#{n} {issue['title'][:70]}: {decision}")
            kind, arg = decision
            if kind == "duplicate":
                self.gh.close(n, "duplicate", dup_of=int(arg))
                return True
            if arg in self.releases:
                self.gh.close(n, "completed")
                return True
            if "fixed-next-release" not in have:
                self.gh.label(n, "fixed-next-release")
            return False
        if c["user"]["login"] == issue["user"]["login"]:
            drop = {"duplicate", "needs-retry"} & have
            if "needs-logs" in have and classify.has_log(c["body"]):
                drop.add("needs-logs")
            if drop:
                print(f"#{n}: its author replied")
                self.gh.unlabel(n, *sorted(drop))
        return False

    def labeled_at(self, n: int, label: str) -> datetime | None:
        events = [e for e in self.gh.get(f"issues/{n}/events?per_page=100") if e["event"] == "labeled"]
        times = [e["created_at"] for e in events if e["label"]["name"] == label]
        return datetime.fromisoformat(times[-1]) if times else None

    def ask_retry(self, issue: dict, comments: list[dict]) -> None:
        """A report on a release older than the newest two, quiet for QUIET_DAYS: asked to try the latest. Not the
        issues a known error points to (they track it), nor those waiting for something else (NOT_RETRIED)."""
        n = issue["number"]
        have = {lb["name"] for lb in issue["labels"]}
        if have & NOT_RETRIED or n in {s.issue for s in self.sigs}:
            return
        old = [lb for lb in have if classify.behind(lb, set(self.releases))]
        if not old or any(MARK.format("retry") in c["body"] for c in comments):
            return
        last = max([issue["created_at"], *(c["created_at"] for c in comments)])
        if datetime.now(UTC) - datetime.fromisoformat(last) < timedelta(days=QUIET_DAYS):
            return
        latest = max(self.releases, key=classify.vkey)
        print(f"#{n} {issue['title'][:70]}: on {old[0]}, quiet since {last[:10]}: asked to retry")
        self.ensure_label("needs-retry")
        self.gh.label(n, "needs-retry")
        self.gh.comment(n, "retry", TEXT["retry"].format(ver=old[0], latest=latest, repo_url=self.repo_url))

    def sweep(self, issue: dict, comments: list[dict]) -> None:
        """Closes what waited long enough for its author: the log, an answer to "looks like #N", or a try on the latest
        release; then asks the old quiet reports for that try."""
        n = issue["number"]
        have = {lb["name"] for lb in issue["labels"]}
        now = datetime.now(UTC)
        for label, days in (("duplicate", DUP_DAYS), ("needs-logs", LOG_DAYS), ("needs-retry", RETRY_DAYS)):
            if label not in have or not (since := self.labeled_at(n, label)) or now - since < timedelta(days=days):
                continue
            author = issue["user"]["login"]
            if any(c["user"]["login"] == author and datetime.fromisoformat(c["created_at"]) > since for c in comments):
                continue
            print(f"#{n} {issue['title'][:70]}: {label} since {since:%d/%m %H:%M}")
            if label == "duplicate":
                bot = "\n".join(c["body"] for c in comments if MARK.format("dup") in c["body"])
                if m := re.search(r"looks like #(\d+)", bot):
                    dup = int(m.group(1))
                    self.gh.close(n, "duplicate", TEXT["dup_closed"].format(issue=dup), dup_of=dup)
                    return
            elif label == "needs-retry":
                self.gh.close(n, "not planned", TEXT["retry_closed"])
                return
            else:
                self.gh.close(n, "not planned", TEXT["logs_closed"])
                return
        self.ask_retry(issue, comments)

    def release(self, tag: str) -> None:
        url = self.releases.get(tag) or f"{self.repo_url}/releases/tag/{tag}"
        for issue in self.gh.get("issues?state=open&labels=fixed-next-release&per_page=100"):
            print(f"#{issue['number']} {issue['title'][:70]}: {tag}")
            self.gh.unlabel(issue["number"], "fixed-next-release")
            self.gh.close(issue["number"], "completed", TEXT["release"].format(tag=tag, url=url))

    def open_issues(self) -> list[dict]:
        return [i for i in self.gh.get("issues?state=open&per_page=100") if "pull_request" not in i]


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("event", choices=["issue", "comment", "sweep", "release", "backlog"])
    ap.add_argument("args", nargs="*")
    ap.add_argument("--repo", default=os.environ.get("GITHUB_REPOSITORY", "adriadam10/mkw-recomp"))
    ap.add_argument("--apply", action="store_true", help="act; without it, print what it would do")
    a = ap.parse_args()
    m = Moderator(GitHub(a.repo, a.apply))
    if a.event == "issue":
        n = int(a.args[0])
        m.on_issue(m.gh.get(f"issues/{n}"), m.comments(n))
    elif a.event == "comment":
        n, cid = int(a.args[0]), int(a.args[1])
        m.on_comment(m.gh.get(f"issues/{n}"), m.gh.get(f"issues/comments/{cid}"))
    elif a.event == "release":
        m.release(a.args[0])
    else:
        for issue in m.open_issues():
            comments = m.comments(issue["number"])
            if a.event == "backlog":
                # The last reply of a maintainer that settles it, unless a player wrote after it (#38: "same error in
                # v0.1.7" under "fixed in v0.1.7"); then labels and first answer like a new issue
                settled = [
                    i
                    for i, c in enumerate(comments)
                    if c["author_association"] in MAINTAINERS and classify.reply_decision(c["body"])
                ]
                after = settled and any(c["author_association"] not in MAINTAINERS for c in comments[settled[-1] + 1 :])
                if settled and not after and m.on_comment(issue, comments[settled[-1]]):
                    continue
                m.on_issue(issue, comments)
            m.sweep(issue, comments)


if __name__ == "__main__":
    main()
