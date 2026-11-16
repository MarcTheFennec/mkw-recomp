"""The GFX9 RADV guard on block-view COLOR_ATTACHMENT (runtime/gfx/vk/tex/vk_tex_gfx9.c), built alone with the C
compiler at hand. RADV on GFX9 (maxImageDimension3D 2048, radv_init_image_properties in Mesa) must make a 3D
block-compressed image with mips and depth in blocks without COLOR_ATTACHMENT — the combination addrlib's swizzle
filter empties into a SIGTRAP inside vkCreateImage (the Vega 7 crash at slot 337) — and every other device must keep
its block views: RDNA (8192), other vendors and drivers, 2D images, one mip, depth below the blocks.

Usage: uv run pytest tests
"""

import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "runtime/gfx/vk/tex/vk_tex_gfx9.c"

MAIN = r"""
#include <stdio.h>
#include <stdlib.h>
#include "vk_tex_gfx9.h"
int main(int argc, char **argv) {
    (void)argc;
    printf("%d\n", vk_tex_gfx9_skip_block_ca(atoi(argv[1]), (unsigned)strtoul(argv[2], 0, 0),
                                              (unsigned)strtoul(argv[3], 0, 0), atoi(argv[4]),
                                              (unsigned)strtoul(argv[5], 0, 0), (unsigned)strtoul(argv[6], 0, 0),
                                              (unsigned)strtoul(argv[7], 0, 0), (unsigned)strtoul(argv[8], 0, 0),
                                              (unsigned)strtoul(argv[9], 0, 0), (unsigned)strtoul(argv[10], 0, 0)));
    return 0;
}
"""

# (name, expected, mesa_radv, vendor, maxImageDimension3D, 3D, mip_levels, depth, width, height, block_w, block_h)
CASES = [
    # the crash's shape: RADV on a Vega 7 (RENOIR), a 32^3 BC1 volume with mips, depth 32 >= ceil(32/4) = 8
    ("RADV GFX9 32x32x32 mipped BC1", 1, 1, 0x1002, 2048, 1, 6, 32, 32, 32, 4, 4),
    # depth exactly at the block counts is already the failing class (addrlib: 4x4x1 with two levels)
    ("RADV GFX9 4x4x1 two levels", 1, 1, 0x1002, 2048, 1, 2, 1, 4, 4, 4, 4),
    ("RADV GFX9 32x32x8 two levels (depth = blocks)", 1, 1, 0x1002, 2048, 1, 2, 8, 32, 32, 4, 4),
    # non-square: the depth must reach both block counts, 64x16 -> 16x4
    ("RADV GFX9 64x16x16 mipped", 1, 1, 0x1002, 2048, 1, 4, 16, 64, 16, 4, 4),
    ("RADV GFX9 64x16x15 mipped (depth 15 < 16)", 0, 1, 0x1002, 2048, 1, 4, 15, 64, 16, 4, 4),
    ("RADV GFX9 32x32x7 two levels (depth 7 < 8)", 0, 1, 0x1002, 2048, 1, 2, 7, 32, 32, 4, 4),
    # RDNA and newer keep their block views: radv_init_image_properties reports 8192 from GFX10, 16384 on GFX12
    ("RADV RDNA 32x32x32 mipped", 0, 1, 0x1002, 8192, 1, 6, 32, 32, 32, 4, 4),
    ("RADV GFX12 32x32x32 mipped", 0, 1, 0x1002, 16384, 1, 6, 32, 32, 32, 4, 4),
    # AMD's proprietary driver (Windows on Vega works) and any non-RADV driver are untouched
    ("not RADV, AMD vendor, GFX9 limit", 0, 0, 0x1002, 2048, 1, 6, 32, 32, 32, 4, 4),
    ("RADV, non-AMD vendor", 0, 1, 0x10DE, 2048, 1, 6, 32, 32, 32, 4, 4),
    # 2D images and single-level volumes never enter the failing class
    ("RADV GFX9 2D 32x32 mipped", 0, 1, 0x1002, 2048, 0, 6, 32, 32, 32, 4, 4),
    ("RADV GFX9 32x32x32 one level", 0, 1, 0x1002, 2048, 1, 1, 32, 32, 32, 4, 4),
    # no division by zero on a malformed block size (the call site only ever passes 4x4)
    ("RADV GFX9 zero block size", 0, 1, 0x1002, 2048, 1, 6, 32, 32, 32, 0, 0),
]


@pytest.fixture(scope="module")
def skip_exe(tmp_path_factory: pytest.TempPathFactory) -> Path:
    cc = shutil.which("clang") or shutil.which("cc") or shutil.which("gcc")
    if not cc:
        pytest.skip("no C compiler")
    d = tmp_path_factory.mktemp("gfx9")
    (d / "main.c").write_text(MAIN)
    exe = d / "gfx9.exe"
    subprocess.run([cc, "-O1", f"-I{SRC.parent}", str(SRC), str(d / "main.c"), "-o", str(exe)], check=True)
    return exe


def skip(exe: Path, case: tuple) -> int:
    args = [str(exe), *(str(v) for v in case[2:])]
    out = subprocess.run(args, capture_output=True, text=True, check=True)
    return int(out.stdout.split()[0])


def test_skip_block_ca(skip_exe: Path) -> None:
    for name, want, *_rest in CASES:
        got = skip(skip_exe, (name, want, *_rest))
        assert got == want, f"{name}: skip = {got}, want {want}"
