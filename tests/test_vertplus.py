"""Exercise the real race-FOV hook after aspect assignment, without an owned game."""

import math
import os
import subprocess

import pytest

from test_aspect import LIBM, ROOT, function


@pytest.fixture(scope="module")
def fov_hook(tmp_path_factory):
    source = (ROOT / "runtime/core/boot/patches.c").read_text()
    code = r"""
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu/cpu.h"
static float aspect_scale;
static struct { int number; uint8_t fov_entry_x, fov_half_v; } version = {100, 24, 13};
#define rt_version() (&version)
"""
    code += (
        function(source, "aspect_fov_angle")
        + function(source, "host_camera_fov")
        + function(source, "aspect_tall_target")
    )
    code += r"""
int main(int argc, char **argv) {
    if (argc == 5) {
        uint32_t wh[2];
        aspect_tall_target(wh, atoi(argv[1]), 2448.0f / 1848, atoi(argv[2]), atoi(argv[3]));
        printf("%u %u\n", wh[0], wh[1]); return 0;
    }
    if (argc != 4) return 2;
    if (getenv("V180")) version.number = 180, version.fov_entry_x = 20, version.fov_half_v = 14;   // version.c
    aspect_scale = strtof(argv[1], 0);
    float requested = strtof(argv[2], 0), half = 0.5f;
    float projection[64] = {0};
    projection[0xb0 / 4] = strtof(argv[3], 0);
    Cpu c = {0};
    c.x[version.fov_entry_x] = (uint64_t)(uintptr_t)projection;
    // Each frame's camera update supplies its fresh requested FOV after setting aspect.
    for (int i = 0; i < 1000; i++) {
        memcpy(&c.v[0], &requested, 4); memcpy(&c.v[version.fov_half_v], &half, 4);
        host_camera_fov(&c);
    }
    float angle, tangent_arg;
    memcpy(&angle, &c.v[0], 4); memcpy(&tangent_arg, &c.v[8], 4);
    printf("%.9g %.9g\n", angle, tangent_arg);
}
"""
    out = tmp_path_factory.mktemp("vertplus")
    (out / "test.c").write_text(code)
    subprocess.run(
        [
            "clang",
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-D_CRT_SECURE_NO_WARNINGS",  # Windows CI: the UCRT deprecates sscanf and friends
            "-I" + str(ROOT / "runtime"),
            str(out / "test.c"),
            *LIBM,
            "-o",
            str(out / "test.exe"),
        ],
        check=True,
    )
    return out / "test.exe"


@pytest.mark.parametrize("v180", [False, True])
@pytest.mark.parametrize("angle", [0.4, math.radians(65), 1.6])
def test_late_race_fov_write_preserves_horizontal_view(fov_hook, angle, v180):
    aspect = 2448 / 1848
    scale = aspect / (16 / 9)
    env = {**os.environ, "V180": "1"} if v180 else None
    result = subprocess.check_output([str(fov_hook), str(scale), str(angle), str(aspect)], text=True, env=env)
    actual, half = map(float, result.split())
    assert actual == pytest.approx(2 * half)
    assert math.tan(half) * aspect == pytest.approx(math.tan(angle / 2) * 16 / 9)
    assert actual > angle


@pytest.mark.parametrize("scale,aspect", [(1, 16 / 9), (2, 32 / 9), (0.745, 1)])
def test_other_projections_keep_the_original_fov(fov_hook, scale, aspect):
    result = subprocess.check_output([str(fov_hook), str(scale), "1.0", str(aspect)], text=True)
    assert list(map(float, result.split())) == [1, 0.5]


@pytest.mark.parametrize("lines,w,h,expected", [(1080, 0, 0, [1920, 1450]), (1440, 2448, 1848, [2448, 1848])])
def test_tall_target_adds_rows_instead_of_removing_columns(fov_hook, lines, w, h, expected):
    result = subprocess.check_output([str(fov_hook), str(lines), str(w), str(h), "target"], text=True)
    assert list(map(int, result.split())) == expected
