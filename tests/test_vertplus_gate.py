"""Old generated game tables must not silently enable a tall crop with a newer runtime."""

import subprocess

import pytest

from test_aspect import ROOT, function


@pytest.fixture(scope="module")
def gate(tmp_path_factory):
    source = (ROOT / "runtime/core/boot/patches.c").read_text()
    code = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/boot/gamelib.h"
static int width, height;
static float aspect_out, aspect_scale;
static struct { int number; const char *display; uint64_t aspect_first_lr; } version = {100, "1.0.0", 0x460d44};
#define rt_version() (&version)
static const char *cfg_value(const char *key) { (void)key; return "window"; }
static void gfx_window_shape(int *w, int *h) { *w = width; *h = height; }
static void gfx_aspect_set(float a) { (void)a; }
static void rt_log(const char *fmt, ...) { (void)fmt; }
"""
    code += "\n".join(function(source, name) for name in ("aspect_of", "has_set", "aspect_set"))
    code += r"""
int main(int argc, char **argv) {
    if (argc != 5) return 2;
    version.number = atoi(argv[4]);
    version.aspect_first_lr = version.number == 100 ? 0x460d44 : version.number == 180 ? 0x8a6d8 : 0;   // version.c
    width = atoi(argv[1]); height = atoi(argv[2]);
    RtPatchSet sets[] = {{"aspect", "MKW_ASPECT_PATCH", 0}, {"vertplus", "MKW_VERTPLUS_PATCH", 0}};
    uint8_t on[RT_MAX_PATCH_SETS] = {0};
    if (atoi(argv[3]) == 2) sets[0].name = "other";
    RtGame g = {.patch_sets = sets, .npatch_sets = atoi(argv[3]) ? 2 : 1, .patch_on = on};
    aspect_set(&g);
    printf("%.9g %u %u\n", aspect_out, on[0], on[1]);
}
"""
    out = tmp_path_factory.mktemp("vertplus-gate")
    (out / "gate.c").write_text(code)
    subprocess.run(
        [
            "clang",
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-D_CRT_SECURE_NO_WARNINGS",  # Windows CI: the UCRT deprecates sscanf and friends
            "-I" + str(ROOT / "runtime"),
            "-I" + str(ROOT / "runtime/cpu"),
            str(out / "gate.c"),
            "-o",
            str(out / "gate.exe"),
        ],
        check=True,
    )
    return out / "gate.exe"


@pytest.mark.parametrize(
    "width,height,has_fov_patch,version,expected",
    [
        (2448, 1848, 0, 100, [0, 0, 0]),
        (2448, 1848, 1, 100, [2448 / 1848, 1, 1]),
        (1920, 1080, 1, 100, [0, 0, 0]),
        (2560, 1080, 1, 100, [2560 / 1080, 1, 0]),
        (2560, 1080, 0, 100, [2560 / 1080, 1, 0]),
        (2448, 1848, 2, 100, [0, 0, 0]),
        (2448, 1848, 1, 180, [2448 / 1848, 1, 1]),
        (2448, 1848, 0, 180, [0, 0, 0]),
        (2448, 1848, 1, 999, [0, 0, 0]),
        (2560, 1080, 1, 180, [2560 / 1080, 1, 0]),
    ],
)
def test_tall_requires_the_matching_generated_fov_patch(gate, width, height, has_fov_patch, version, expected):
    out = subprocess.check_output([str(gate), str(width), str(height), str(has_fov_patch), str(version)], text=True)
    assert list(map(float, out.split())) == pytest.approx(expected)
