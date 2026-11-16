"""Fill new or changed menu texts in every catalog language (i18n/<lang>.po) from a JSON table.

Why: a new English text in mkw.pot leaves twenty .po entries empty, and a changed one marks them fuzzy; editing twenty
files by hand loses printf conversions and wrapped lines. The table is {msgctxt: {lang: text}}; only an empty entry
or a fuzzy one (its English changed) is written, and a written fuzzy one loses its flag and old English. A checked
translation (not fuzzy, not empty) is never overwritten. catalog.py's own reader and writer keep the file's format.

Usage: uv run tools/i18n/fill.py TABLE.json        (after `uv run tools/i18n/catalog.py update`)
"""

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
I18N = ROOT / "i18n"
sys.path.insert(0, str(ROOT / "tools/i18n"))
import catalog  # noqa: E402


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    table: dict[str, dict[str, str]] = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
    langs = sorted({lang for t in table.values() for lang in t})
    changed = 0
    for lang in langs:
        path = I18N / f"{lang}.po"
        if not path.exists():
            print(f"fill: no {path.name}", file=sys.stderr)
            return 1
        header, entries = catalog.parse(path)
        for e in entries:
            text = table.get(e.ctx, {}).get(lang)
            if text is None or (e.msgstr and not e.fuzzy):
                continue
            e.msgstr, e.fuzzy, e.prev = text.replace('"', '\\"'), False, None
            changed += 1
        catalog.write(path, header, entries)
    print(f"fill: {changed} entries written in {len(langs)} languages")
    return 0


if __name__ == "__main__":
    sys.exit(main())
