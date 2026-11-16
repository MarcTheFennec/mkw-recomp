"""Refuse a commit whose docs no longer match the repo: every folder documented, links alive, generated blocks fresh.

Why: a README per folder, updated in the same change that adds, moves or removes something in it, only held once it
was checked: on 05/11/2026 nine folders had no README, ten READMEs left out files of their folder and the root README
described the state of five days before. Checks, on the files git tracks (index):
  1. Every folder has a README.md, except third-party code (a folder with a LICENSE file, and below it).
  2. That README names each file and subfolder of its folder: by name, by a backticked glob (`*_spv.h`) or as `dir/`.
  3. Every relative link `[x](path)` of a .md points to a file that exists.
  4. The progress bars of README.md (progress.py), when it has them, are the generated ones (cog).
  5. A commit that touches the shader translator (sm86/) carries the measurements behind those bars: with build/spirv
     present, build/spirv/failed.tsv is no older than any program result (translate.py rewrites it; retry_shas.py does
     not) and tools/docs/progress.json counts the programs translated now (06/11/2026: 20 more programs went in with
     the README still at 11 511).
  6. A note, not a refusal, when a translation of the README (docs/i18n/README.<lang>.md) was made from an older
     README.md than the one committed (tools/docs/i18n_readme.py --check; Adrià 13/11/2026: it warns).
  7. The table of docs/performance.md is sorted by version, then by internal resolution (perf_table.py --check).

Usage: uv run tools/docs/check.py          (tools/git/pre-commit and the CI run it; exit 1 lists every problem)
"""

import fnmatch
import io
import json
import re
import subprocess
import sys
from pathlib import Path, PurePosixPath

from cogapp import Cog

ROOT = Path(__file__).resolve().parents[2]
LINK = re.compile(r"\]\(([^)\s]+)\)")
BACKTICK = re.compile(r"`([^`\s]+)`")


def git(*args: str) -> list[str]:
    out = subprocess.run(["git", "-C", str(ROOT), *args], check=True, capture_output=True, text=True).stdout
    return [line for line in out.split("\0" if "-z" in args else "\n") if line]


def tree(files: list[str]) -> dict[str, set[str]]:
    """Folder ('' for the root) -> names of its direct children, subfolders ending in '/'."""
    children: dict[str, set[str]] = {}
    for f in files:
        parts = f.split("/")
        for i, name in enumerate(parts):
            folder = "/".join(parts[:i])
            children.setdefault(folder, set()).add(name + ("/" if i < len(parts) - 1 else ""))
    return children


def exempt(folder: str, children: dict[str, set[str]]) -> bool:
    """Folders that need no README of their own (see check 1)."""
    p = PurePosixPath(folder)
    if p.parts[:1] == (".github",):
        return True
    for anc in [p, *p.parents]:
        names = children.get(str(anc) if str(anc) != "." else "", set())
        if "README.md" not in names and any(n.startswith("LICENSE") for n in names):
            return True
    return False


def named(name: str, text: str, globs: list[str]) -> bool:
    bare = name.rstrip("/")
    if re.search(r"(?<![\w./-])" + re.escape(bare) + r"(?![\w-]|\.\w)", text):
        return True
    return any(fnmatch.fnmatchcase(bare, g) for g in globs)


def check_folders(children: dict[str, set[str]]) -> list[str]:
    errors = []
    for folder, names in sorted(children.items()):
        if "README.md" not in names:
            if not exempt(folder, children):
                errors.append(f"{folder or '.'}/: no README.md")
            continue
        text = (ROOT / folder / "README.md").read_text()
        globs = [g for g in BACKTICK.findall(text) if "*" in g]
        missing = sorted(n for n in names if n != "README.md" and not named(n, text, globs))
        if missing:
            errors.append(f"{folder + '/' if folder else ''}README.md: does not name {', '.join(missing)}")
    return errors


def check_links(files: list[str]) -> list[str]:
    errors = []
    for md in files:
        # the README's translated bodies link as from the root: their links are checked in the built README.<lang>.md
        if not md.endswith(".md") or md.startswith("docs/i18n/body/"):
            continue
        for target in LINK.findall((ROOT / md).read_text()):
            if re.match(r"[a-z]+:|#", target):
                continue
            path = (ROOT / md).parent / target.split("#")[0]
            if not path.exists():
                errors.append(f"{md}: broken link {target}")
    return errors


def check_readme() -> list[str]:
    """The generated block of the root README, when it has one: the progress bars (progress.py)."""
    if "[[[cog" not in (ROOT / "README.md").read_text():
        return []
    cog, out = Cog(), io.StringIO()
    cog.set_output(stdout=out, stderr=out)
    if cog.main(["cog", "--check", "-I", str(ROOT / "tools" / "docs"), str(ROOT / "README.md")]) != 0:
        return ["README.md: generated block out of date; run `uv run cog -r -I tools/docs README.md`"]
    return []


def check_progress(staged: set[str]) -> list[str]:
    spirv = ROOT / "build" / "spirv"
    if not any(f.startswith("sm86/") for f in staged) or not (spirv / "failed.tsv").exists():
        return []
    results = list(spirv.glob("*.result"))
    newest = max((r.stat().st_mtime for r in results), default=0)
    if (spirv / "failed.tsv").stat().st_mtime < newest:
        return ["build/spirv/failed.tsv is older than a program result: run `uv run tools/shaders/translate.py`"]
    ok = sum(1 for r in results if r.read_text().strip() == "ok")
    recorded = json.loads((ROOT / "tools" / "docs" / "progress.json").read_text())["programs"]["ok"]
    if recorded != ok:
        msg = (
            f"tools/docs/progress.json counts {recorded} programs translated, build/spirv has {ok}: run "
            "`uv run tools/docs/progress.py --update --census build/run/NAME.imports` and cog on README.md"
        )
        return [msg]
    return []


def check_perf_table() -> list[str]:
    r = subprocess.run([sys.executable, str(ROOT / "tools/docs/perf_table.py"), "--check"], capture_output=True, text=True)
    return [r.stderr.strip().removeprefix("docs: ")] if r.returncode else []


def note_translations() -> None:
    sys.path.insert(0, str(ROOT / "tools/docs"))
    import i18n_readme

    for p in i18n_readme.stale():
        print(f"docs: note: {p} was translated from an older README.md: update docs/i18n/body/ and run "
              "tools/docs/i18n_readme.py", file=sys.stderr)  # fmt: skip


def main() -> int:
    files = git("ls-files", "-z")
    staged = set(git("diff", "--cached", "--name-only", "--diff-filter=AM"))
    errors = check_folders(tree(files)) + check_links(files) + check_progress(staged) + check_readme() + check_perf_table()
    for e in errors:
        print(f"docs: {e}", file=sys.stderr)
    note_translations()
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
