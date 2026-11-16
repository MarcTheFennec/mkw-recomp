"""DSP-ADPCM voices (runtime/audio/audio_mixer.c, its "voice decoding" section) against an exact decode of the same
random buffers (tests/c/t_adpcm.c), built with the C compiler at hand. With a context on every buffer the output must
be the exact decode, sample for sample, loops included; without contexts the history carried from the buffer before is
our model of the DSP, and a few samples still differ (a slot refilled before the walk decoded its last samples). Each
sample is decoded about once, not about three times as before #331.

Usage: uv run pytest tests
"""

import os
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
MIXER = ROOT / "runtime/audio/audio_mixer.c"
FIRST = "/* ---------- voice decoding along the voice clock's sample path ---------- */"
NEXT = "/* ---------- mixing ---------- */"
DECODE = "static int16_t adpcm_next(Walk *w, const AudrenWaveBuffer *b, const int16_t *coef, uint64_t i) {"


@pytest.fixture(scope="module")
def adpcm(tmp_path_factory: pytest.TempPathFactory) -> Path:
    cc = shutil.which("clang") or shutil.which("cc") or shutil.which("gcc")
    if not cc:
        pytest.skip("no C compiler")
    src = MIXER.read_text(encoding="utf-8")
    assert FIRST in src and NEXT in src and DECODE in src, "audio_mixer.c changed: update test_adpcm.py"
    section = src[src.index(FIRST) : src.index(NEXT)].replace(DECODE, DECODE + " decodes++;")
    d = tmp_path_factory.mktemp("adpcm")
    (d / "voice.inc").write_text(section, encoding="utf-8")
    exe = d / "t_adpcm.exe"
    inc = [f"-I{d}", f"-I{ROOT / 'runtime'}", f"-I{ROOT / 'runtime/audio'}", f"-I{ROOT / 'runtime/core/base'}",
           f"-I{ROOT / 'runtime/cpu'}"]  # fmt: skip
    libm = [] if os.name == "nt" else ["-lm"]
    subprocess.run([cc, "-O1", *inc, str(ROOT / "tests/c/t_adpcm.c"), "-o", str(exe), *libm], check=True)
    return exe


def run(exe: Path, mode: str) -> tuple[int, int, int]:
    out = subprocess.run([str(exe), mode], capture_output=True, text=True, check=True)
    outs, wrong, decodes = (int(x) for x in out.stdout.split())
    return outs, wrong, decodes


def test_with_contexts_the_output_is_the_exact_decode(adpcm: Path) -> None:
    outs, wrong, decodes = run(adpcm, "ctx")
    assert outs == 400 * 300 * 240
    assert wrong == 0
    assert decodes < outs * 1.5  # a loop decodes again from the buffer's start to its loop start


def test_without_contexts_the_history_carries_over(adpcm: Path) -> None:
    outs, wrong, decodes = run(adpcm, "mixed")
    assert wrong < outs // 100000  # 14 of 28.8 M: a slot refilled before the walk decoded its last samples
    assert decodes < outs * 1.1  # 1.04 (3.08 before #331)


def test_high_pitch_finishes_the_previous_buffer(adpcm: Path) -> None:
    outs, wrong, _ = run(adpcm, "high")
    assert outs == 400 * 300 * 240
    assert wrong < 5500  # 6046 with a fixed 128-sample limit; remaining differences come from early slot refills
