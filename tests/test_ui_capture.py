"""Compile the real capture-mask classification with authored pane fixtures; no game required."""

import subprocess

import pytest

from test_aspect import LIBM, ROOT, function


@pytest.fixture(scope="module")
def capture_strip(tmp_path_factory):
    source = (ROOT / "runtime/core/boot/patches.c").read_text()
    code = r"""
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu/cpu.h"
static float aspect_scale;
static unsigned char pane[0xe0];
static int plat_mem_readable(uint64_t p, size_t n) {
    return p >= (uint64_t)(uintptr_t)pane && p + n <= (uint64_t)(uintptr_t)(pane + sizeof pane);
}
"""
    code += function(source, "ui_tall_backdrop")
    code += r"""
int main(int argc, char **argv) {
    if (argc != 6) return 2;
    aspect_scale = strtof(argv[1], 0);
    float size[2] = {strtof(argv[2], 0), strtof(argv[3], 0)};
    memcpy(pane + 0x50, size, sizeof size);
    snprintf((char *)pane + 0xb0, 32, "%s", argv[4]);
    Cpu c = {0};
    c.x[23] = (uint64_t)(uintptr_t)(pane + 0x70);
    c.x[22] = (uint64_t)(uintptr_t)(pane + 0x50 + atoi(argv[5]));
    printf("%d\n", ui_tall_backdrop(&c));
}
"""
    root = tmp_path_factory.mktemp("capture-strip")
    (root / "test.c").write_text(code)
    subprocess.run(
        [
            "clang",
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-D_CRT_SECURE_NO_WARNINGS",  # Windows CI: the UCRT deprecates sscanf and friends
            "-I" + str(ROOT / "runtime"),
            str(root / "test.c"),
            *LIBM,
            "-o",
            str(root / "test.exe"),
        ],
        check=True,
    )
    return root / "test.exe"


@pytest.mark.parametrize("height", [1080, 954.8])
@pytest.mark.parametrize("name", ["P_Cap_00", "P_Cap_01"])
def test_tall_character_and_kart_capture_masks_follow_backdrop(capture_strip, height, name):
    assert subprocess.check_output([str(capture_strip), "0.745", "100", str(height), name, "0"], text=True) == "1\n"


@pytest.mark.parametrize(
    "scale,width,height,name,offset",
    [
        (1, 100, 1080, "P_Cap_00", 0),
        (2, 100, 1080, "P_Cap_00", 0),
        (0, 100, 1080, "P_Cap_00", 0),
        (0.745, 100, 1080, "P_Base_00", 0),
        (0.745, 100, 100, "P_Cap_00", 0),
        (0.745, 1920, 1080, "P_Cap_00", 0),
        (0.745, 100, 1080, "P_Cap_00", 4),
    ],
)
def test_other_panes_and_nontall_outputs_keep_their_projection(capture_strip, scale, width, height, name, offset):
    assert (
        subprocess.check_output([str(capture_strip), str(scale), str(width), str(height), name, str(offset)], text=True)
        == "0\n"
    )


def test_tall_cup_side_backdrop_covers_the_added_height(capture_strip):
    assert subprocess.check_output([str(capture_strip), "0.745", "1080", "1080", "P_BG_00", "0"], text=True) == "1\n"


@pytest.fixture(scope="module")
def projection_hook(tmp_path_factory):
    source = (ROOT / "runtime/core/boot/patches.c").read_text()
    code = r"""
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu/cpu.h"
static float aspect_scale, aspect_out;
static uint64_t split_at;
static int movie;
static struct { uint64_t ui_text_lr; } version;
#define rt_version() (&version)
static uint64_t rt_main_addr(uint64_t p) { return p; }
static uint64_t frame_step_ticks(void) { return 1; }
static int ov_movie_playing(void) { return movie; }
static int seqwatch_probe(void) { return 0; }
#define rt_log(...) ((void)0)
static unsigned char pane[0xe0];
static int plat_mem_readable(uint64_t p, size_t n) {
    return p >= (uint64_t)(uintptr_t)pane && p + n <= (uint64_t)(uintptr_t)(pane + sizeof pane);
}
"""
    code += "\n".join(
        function(source, name)
        for name in (
            "is_16x9",
            "ui_full_screen",
            "aspect_ui_fit",
            "ui_tall_backdrop",
            "host_ui_proj",
        )
    )
    code += r"""
int main(int argc, char **argv) {
    if (argc != 6) return 2;
    aspect_scale = strtof(argv[1], 0); aspect_out = aspect_scale * (16.0f / 9);
    float size[2] = {strtof(argv[2], 0), strtof(argv[3], 0)};
    memcpy(pane + 0x50, size, sizeof size);
    snprintf((char *)pane + 0xb0, 32, "%s", argv[4]);
    movie = atoi(argv[5]);
    float matrix[12] = {1,0,0,0, 0,1,0,0, 0,0,1,0};
    memcpy(pane + 0x70, matrix, sizeof matrix);
    float info[32] = {0}, out[16] = {0};
    info[4] = 1; info[9] = 16.0f / 9;
    Cpu c = {0}; c.x[0] = (uint64_t)(uintptr_t)info; c.x[1] = (uint64_t)(uintptr_t)out;
    c.x[23] = (uint64_t)(uintptr_t)(pane + 0x70); c.x[22] = c.x[23] - 0x20;
    for (int i = 0; i < 1000; i++) host_ui_proj(&c);
    printf("%.9g %.9g %.9g %.9g\n", info[4], info[9], out[0], out[5]);
}
"""
    root = tmp_path_factory.mktemp("projection-hook")
    (root / "test.c").write_text(code)
    subprocess.run(
        [
            "clang",
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-D_CRT_SECURE_NO_WARNINGS",  # Windows CI: the UCRT deprecates sscanf and friends
            "-I" + str(ROOT / "runtime"),
            str(root / "test.c"),
            *LIBM,
            "-o",
            str(root / "test.exe"),
        ],
        check=True,
    )
    return root / "test.exe"


@pytest.mark.parametrize(
    "width,height,name,expanded",
    [
        (100, 1080, "P_Cap_00", True),
        (100, 954.8, "P_Cap_01", True),
        (1080, 1080, "P_BG_00", True),
        (1920, 1080, "P_Capture_00", True),
        (100, 1080, "P_Base_00", False),
        (256, 340, "P_Base_02", False),
    ],
)
def test_actual_draw_hook_keeps_backdrops_and_controls_separate(projection_hook, width, height, name, expanded):
    result = subprocess.check_output([str(projection_hook), "0.745", str(width), str(height), name, "0"], text=True)
    sx, sy, dx, dy = map(float, result.split())
    assert sx == dx == 1
    assert sy == pytest.approx(16 / 9 * 0.745)
    assert dy == pytest.approx(16 / 9 * (1 if expanded else 0.745))


@pytest.mark.parametrize("scale", [1, 2])
def test_actual_draw_hook_does_not_expand_masks_in_nontall_windows(projection_hook, scale):
    result = subprocess.check_output([str(projection_hook), str(scale), "100", "1080", "P_Cap_00", "0"], text=True)
    assert list(map(float, result.split())) == pytest.approx([1 / scale, 16 / 9, 1 / scale, 16 / 9])


def test_movie_keeps_capture_masks_fitted(projection_hook):
    result = subprocess.check_output([str(projection_hook), "0.745", "100", "1080", "P_Cap_00", "1"], text=True)
    assert list(map(float, result.split())) == pytest.approx([1, 16 / 9 * 0.745, 1, 16 / 9 * 0.745])
