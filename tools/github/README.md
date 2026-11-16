# github

Issue moderation (`moderate.py`, `classify.py`), run by `.github/workflows/issues.yml` on each issue event, once a day and on each release. The copy to the Internet Archive (`archive.py`) runs from `.github/workflows/archive.yml` once a day.

| File | Contents |
|---|---|
| `moderate.py` | Acts on an issue, a comment, a release or the daily sweep: release and GPU labels, empty reports closed, logs asked for, known errors answered, a log from a build with local changes (`-dirty`) noted, a known error only in an older log (`mkw.N.log`) or only the setup's warm-up log attached noted without closing, maintainers' replies closed. Prints what it would do unless `--apply` |
| `classify.py` | Reads a report without GitHub: release, GPU, empty, log, known error, and what a maintainer's reply decides (tested in `tests/test_issues.py`) |
| `archive.py` | The daily copy to the Internet Archive (`.github/workflows/archive.yml`): git bundle and every issue, pull request, discussion and release as JSON in a dated item, each release's files once, and the changed pages to the Wayback Machine. Prints what it would do unless `--apply` |
| `signatures.toml` | Known error strings and the issue that tracks each; `fixed` keeps reports on that release or later open as possible regressions; `wiki` is its section in the [wiki's Troubleshooting](https://github.com/adriadam10/mkw-recomp/wiki/Troubleshooting) page, which the bot's reply links |

A known error closes the new issue at once as its duplicate (its author's reply reopens it). A report on a release
older than the newest two, quiet for 3 days, is asked to try the latest (`needs-retry`) and closed after 7 days without
its author's reply. The sweep runs every 6 hours.

A maintainer's reply closes its issue when it says "Fixed in vX" opening a sentence (or "is the same as #N, fixed in
vX") with vX published, or "is the same as #N" (closed as its duplicate). "Fixed in" a release not yet out labels it
`fixed-next-release`, closed when that release is published. A reply only settles an issue if no player wrote after
it, and a closed issue is reopened when its author, or anyone attaching a log, says it still happens.

Usage: `uv run tools/github/moderate.py backlog` (all open issues, dry run), `... issue N --apply`; from GitHub,
`gh workflow run issues.yml -f task=backlog` (or `sweep`, `issue -f issue=N`; `-f dry_run=true` only prints).
