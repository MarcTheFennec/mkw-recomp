# tools/docs

Keeping the docs from going stale: what used to be copied by hand is generated, and the rest is checked at commit time.

| Script | What it does | Usage |
|---|---|---|
| `check.py` | Called by `tools/git/pre-commit` and the CI; rejects the commit if a folder has no README (except third-party code with its `LICENSE` and `.github/`), a README does not name one of its files or subfolders, a relative link of a `.md` does not exist, the README's progress bars are not the generated ones, or a translator change does not carry its measurements | `uv run tools/docs/check.py` |
| `perf_table.py` | Sorts the table of `docs/performance.md` by version (`pre-release`, the numbered ones, `source`), then by internal resolution; `check.py` refuses a commit with it unsorted | `uv run tools/docs/perf_table.py [--check]` |
| `i18n_readme.py` | Builds the README's translations (`docs/i18n/README.<lang>.md`) from each language's body in `docs/i18n/body/`: the centred title, download buttons, link row and language bar every language repeats, the body's links moved two folders up, and the hash of the `README.md` it was translated from; `--check` (also run by `check.py`, as a note) lists the translations older than `README.md` | `uv run tools/docs/i18n_readme.py [--check]` |
| `progress.py`, `progress.json` | Progress bars of the root README (cog block): NVN2 implemented, `main`'s `nn::*` imports replaced and tested, SM86 opcodes translated. What is done is counted in the code; the game's totals (names, imports, census, failures) are in `progress.json`, which `--update` rebuilds from `build/` | `uv run tools/docs/progress.py --update` and `uv run cog -r -I tools/docs README.md` |
