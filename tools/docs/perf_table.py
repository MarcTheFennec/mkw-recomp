#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.12"
# ///
"""Sorts the table of docs/performance.md: by version, then by internal resolution (Adrià 16/11/2026).

Why: a row is added per report, in the order the reports come, so versions and resolutions ended up mixed and two
machines could not be compared at a glance. The order: `pre-release`, then the numbered versions (0.1.6 < 0.1.10),
then `source` (a build from source, not a release); inside a version the lowest internal resolution first (`720p to
2160p` counts as 720p); rows that tie keep the order they were added in. check.py runs `--check` at commit time.

Usage: uv run tools/docs/perf_table.py           (rewrites the table in place)
       uv run tools/docs/perf_table.py --check   (exit 1 if the table is not sorted)
"""

import re
import sys
from pathlib import Path

DOC = Path(__file__).resolve().parents[2] / "docs/performance.md"
VERSION, RESOLUTION = 0, 4   # the columns, counting from the first (| Version | GPU | CPU | OS | Internal resolution |)


def key(row: str) -> tuple:
    cells = [c.strip() for c in row.strip().strip("|").split("|")]
    v = cells[VERSION].lower()
    if v == "pre-release":
        ver = (0,)
    elif v == "source":
        ver = (2,)
    else:
        ver = (1, *(int(n) for n in re.findall(r"\d+", v)))
    res = re.search(r"\d+", cells[RESOLUTION])
    return ver, int(res.group()) if res else 1 << 30


def split(lines: list[str]) -> tuple[int, int]:
    """The first and last data row of the table (its header and separator come before)."""
    head = next(i for i, ln in enumerate(lines) if ln.startswith("| Version |"))
    end = head + 2
    while end < len(lines) and lines[end].startswith("|"):
        end += 1
    return head + 2, end


def main() -> int:
    lines = DOC.read_text().split("\n")
    a, b = split(lines)
    rows = sorted(lines[a:b], key=key)   # sorted() is stable: ties stay in the order they were added
    if rows == lines[a:b]:
        return 0
    if "--check" in sys.argv:
        print("docs: docs/performance.md: table not sorted by version and resolution; run `uv run tools/docs/perf_table.py`",
              file=sys.stderr)
        return 1
    DOC.write_text("\n".join(lines[:a] + rows + lines[b:]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
