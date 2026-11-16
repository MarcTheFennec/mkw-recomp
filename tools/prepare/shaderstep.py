"""The shaders step of prepare.py: the SM86 spec built here from NVIDIA's nvdisasm, then the game's shaders extracted
from the RomFS and translated to SPIR-V; and the step's key.

Why apart: prepare.py holds the order of the steps and their keys; this step drives its own tools. Not run on its own.
"""

import hashlib
import json
import sys
from pathlib import Path

from machine import Failed, crt_spec, run, tool_env

ROOT = Path(__file__).resolve().parents[2]  # the repository, or the package's setup/src


def translator_key() -> str:
    """The shader step's key: every file the translation's output depends on (the translator, the decoder, extract.py
    and translate.py), as translate.py's translator_hash minus the spec, which this step builds. With translate.py
    alone, an update that changed only sm86/translate kept the old SPIR-V (#26: 0.1.7's translator fixes and the
    position default never reached a set-up game)."""
    h = hashlib.sha256()
    files = sorted((ROOT / "sm86/translate").rglob("*.py")) + sorted((ROOT / "sm86/decode").rglob("*.py"))
    for f in [*files, ROOT / "tools/shaders/extract.py", ROOT / "tools/shaders/translate.py"]:
        h.update(f.read_bytes())
    return h.hexdigest()[:16]


def spec_loads(spec: Path) -> bool:
    """Whether the decoder accepts this spec (0.3 s). Until 0.1.12 a capture the parser rejected still left its
    instructions.json behind, and every later setup failed on it in translate.py (issue #325)."""
    sys.path.insert(0, str(ROOT / "sm86/decode"))
    import sass_spec  # noqa: PLC0415 - sm86/decode is not a package

    try:
        sass_spec.Spec(str(spec))
    except (ValueError, KeyError, TypeError) as e:
        print(f"{spec} is broken ({type(e).__name__}: {str(e)[:200]}): building it again", flush=True)
        return False
    return True


def sm86_spec(game: Path, work: Path, cc: list[str]) -> Path:
    """The spec translate.py decodes with: a source checkout's own build/sm86, else built once into <game>/sm86."""
    for spec in (ROOT / "build/sm86/instructions.json", game / "sm86/instructions.json"):
        if spec.is_file() and spec_loads(spec):
            return spec
    crt = crt_spec(game, cc)  # spec.py links a DLL too on Windows: without it, zig builds its libc there (issue #2)
    run(spec_command(game, work, cc, crt))
    return game / "sm86/instructions.json"


def spec_command(game: Path, work: Path, cc: list[str], crt: Path | None) -> list[str]:
    """spec.py's command line; the compiler goes as JSON, whose paths may have spaces (issue #29)."""
    return [sys.executable, str(ROOT / "sm86/spec/spec.py"), "--out", str(game / "sm86"), "--cache",
            str(work / "nvidia"), "--cc", json.dumps(cc), *(["--crt", str(crt)] if crt else [])]  # fmt: skip


def shaders(exefs: Path, romfs: Path, game: Path, work: Path, glslc: str, jobs: int, cc: list[str]) -> None:
    spec = sm86_spec(game, work, cc)
    sh = work / "shaders"
    run([sys.executable, str(ROOT / "tools/shaders/extract.py"), "--romfs", str(romfs), "--exefs", str(exefs),
         "--out", str(sh)])  # fmt: skip
    while True:
        try:
            run([sys.executable, str(ROOT / "tools/shaders/translate.py"), "--shaders", str(sh),
                 "--out", str(game / "spirv"), "--glslc", glslc, "--no-validate", "--progress", "--jobs", str(jobs),
                 "--spec", str(spec)], env=tool_env(glslc), progress_prefix="progress")  # fmt: skip
            break
        except Failed as e:
            # A worker that dies (the system ends it when memory runs out) breaks the whole pool (issue #24,
            # BrokenProcessPool). What is translated stays (translate.py resumes); half the workers need half the memory.
            if "BrokenProcessPool" not in str(e) or jobs == 1:
                raise
            jobs = max(1, jobs // 2)
            print(f"shaders: a worker ended abruptly, again with {jobs} workers", file=sys.stderr)
    for p in (game / "spirv").glob("*.glsl"):  # the runtime reads .spv and .res.json; the GLSL only made them
        p.unlink()
