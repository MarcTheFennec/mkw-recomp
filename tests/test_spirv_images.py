"""Integer-image shader variants: synthetic SPIR-V, production C, no game or GPU.

Exercise storage and separate sampled images, signed/unsigned views and failure diagnostics. When spirv-val is on
PATH, validate both the original and patched modules as well. Usage: uv run pytest tests/test_spirv_images.py
"""

import shutil
import struct
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]


def op(code: int, *args: int) -> list[int]:
    return [((len(args) + 1) << 16) | code, *args]


def shader(*, sampled: bool = False, combined: bool = False, scalar: bool = False,
           already_integer: bool = False) -> list[int]:
    """A compute shader reading binding 4; the sampled form constructs an image from a separate sampler."""
    w = [0x07230203, 0x00010300, 0, 32, 0]
    w += op(17, 1)  # Shader
    if not sampled:
        w += op(17, 55) + op(17, 56)  # StorageImageRead/WriteWithoutFormat
    w += op(14, 0, 1)  # Logical GLSL450
    w += op(15, 5, 20, 0x6E69616D, 0) + op(16, 20, 17, 1, 1, 1)  # main, LocalSize
    w += op(71, 10, 34, 0) + op(71, 10, 33, 4)  # DescriptorSet, Binding
    if sampled and not combined:
        w += op(71, 13, 34, 0) + op(71, 13, 33, 5)
    w += op(19, 1) + op(33, 2, 1)  # void, function
    w += (op(21, 3, 32, 0) if already_integer else op(22, 3, 32)) + op(23, 4, 3, 4)
    w += op(25, 5, 3, 1, 0, 0, 0, 1 if sampled else 2, 0)  # float image2D
    if sampled:
        w += op(27, 14, 5)
    w += op(32, 6, 0, 14 if combined else 5) + op(59, 6, 10, 0)  # UniformConstant pointer, variable
    if sampled:
        if not combined:
            w += op(26, 11) + op(32, 12, 0, 11) + op(59, 12, 13, 0)  # separate sampler
        w += op(23, 15, 3, 2) + op(43, 3, 16, 0) + op(44, 15, 17, 16, 16)
    else:
        w += op(21, 11, 32, 1) + op(23, 12, 11, 2) + op(43, 11, 16, 0) + op(44, 12, 17, 16, 16)
    w += op(54, 1, 20, 0, 2) + op(248, 21) + op(61, 14 if combined else 5, 22, 10)
    if sampled:
        if not combined:
            w += op(61, 11, 23, 13) + op(86, 14, 24, 22, 23)
        w += op(88, 4, 25, 22 if combined else 24, 17, 2, 16)  # sample at explicit LOD 0
    else:
        w += op(98, 3 if scalar else 4, 25, 22, 17)
        w += op(99, 22, 17, 25)
    return w + op(253) + op(56)


@pytest.fixture(scope="module")
def patcher(tmp_path_factory: pytest.TempPathFactory) -> Path:
    cc = shutil.which("clang")
    if not cc:
        pytest.skip("Clang is required by the runtime headers")
    exe = tmp_path_factory.mktemp("spirv-images") / "patch.exe"
    includes = [ROOT / "runtime", ROOT / "runtime/cpu", ROOT / "runtime/core/base",
                ROOT / "runtime/gfx/nvn/host", ROOT / "runtime/gfx/nvn/api", ROOT / "runtime/gfx/nvn/procs"]
    subprocess.run([cc, "-O1", *[f"-I{p}" for p in includes], str(ROOT / "tests/c/t_spirv_images.c"),
                    str(ROOT / "runtime/gfx/nvn/host/gfx_spirv_img.c"), "-o", str(exe)], check=True)
    return exe


def validate(path: Path) -> None:
    validator = shutil.which("spirv-val")
    if validator:
        p = subprocess.run([validator, "--target-env", "vulkan1.1", str(path)], capture_output=True, text=True)
        assert p.returncode == 0, p.stderr


def run(patcher: Path, tmp_path: Path, words: list[int], unsigned: int = 16, signed: int = 0):
    src, dst = tmp_path / "input.spv", tmp_path / "output.spv"
    data = struct.pack(f"<{len(words)}I", *words)
    src.write_bytes(data)
    result = subprocess.run([str(patcher), str(src), str(dst), str(unsigned), str(signed)],
                            capture_output=True, text=True)
    assert src.read_bytes() == data
    return result, src, dst


@pytest.mark.parametrize("mode", ["storage", "separate", "combined"])
@pytest.mark.parametrize("signed", [False, True])
def test_integer_image_variant(patcher: Path, tmp_path: Path, mode: str, signed: bool) -> None:
    words = shader(sampled=mode != "storage", combined=mode == "combined")
    p, src, dst = run(patcher, tmp_path, words, 0 if signed else 16, 16 if signed else 0)
    validate(src)
    assert p.returncode == 0, p.stderr
    assert dst.read_bytes() != src.read_bytes()
    validate(dst)


@pytest.mark.parametrize(("words", "reason"), [
    ([], "invalid SPIR-V header"),
    ([0x07230203, 0x00010300, 0, 4, 0, 0], "malformed SPIR-V instruction"),
    (shader(scalar=True), "image read is not float32 vec4"),
])
def test_failed_patch_explains_why(patcher: Path, tmp_path: Path, words: list[int], reason: str) -> None:
    p, _, dst = run(patcher, tmp_path, words)
    assert p.returncode == 1 and reason in p.stderr
    assert not dst.exists()


def test_no_matching_binding_is_explained(patcher: Path, tmp_path: Path) -> None:
    p, _, _ = run(patcher, tmp_path, shader(), unsigned=1 << 7)
    assert p.returncode == 1 and "no matching float image binding" in p.stderr


def test_correct_integer_image_is_unchanged_without_float_types(patcher: Path, tmp_path: Path) -> None:
    p, src, dst = run(patcher, tmp_path, shader(already_integer=True))
    assert p.returncode == 0, p.stderr
    assert dst.read_bytes() == src.read_bytes()
    validate(src)
    validate(dst)


def test_other_integer_signedness_is_not_a_successful_noop(patcher: Path, tmp_path: Path) -> None:
    p, _, dst = run(patcher, tmp_path, shader(already_integer=True), unsigned=0, signed=16)
    assert p.returncode == 1
    assert not dst.exists()
