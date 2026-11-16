"""Compile the production Window math without a game or GPU: pytest tests/test_aspect.py.

The functions come from patches.c, so the fixture checks the implementation, not a second formula in C.
"""

import math
import os
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
LIBM = [] if os.name == "nt" else ["-lm"]  # Windows has no separate libm (LNK1181: cannot open m.lib)


def function(source: str, name: str) -> str:
    start = source.index("static ", source.rfind("\n", 0, source.index(name + "(")))
    opening = source.index("{", start)
    level = 1
    end = opening + 1
    while level:
        level += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


@pytest.fixture(scope="module")
def aspect(tmp_path_factory):
    cc = shutil.which("clang") or shutil.which("cc")
    if not cc:
        pytest.skip("no C compiler")
    source = (ROOT / "runtime/core/boot/patches.c").read_text()
    code = r"""
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "cpu/cpu.h"
static const char *choice;
static int width, height;
static float aspect_scale;
static volatile uint64_t split_at;
static uint64_t frame_step_ticks(void) { return 1; }
static uint64_t rt_main_addr(uint64_t offset) { return 0x7000000000ull + offset; }
static struct { int number; const char *display; uint64_t aspect_first_lr; uint8_t fov_entry_x, fov_half_v; } version;
static const char *cfg_value(const char *key) { (void)key; return choice; }
static void gfx_window_shape(int *w, int *h) { *w = width; *h = height; }
#define rt_version() (&version)
static void rt_log(const char *fmt, ...) { (void)fmt; }
"""
    code += "\n".join(
        function(source, n) for n in ("aspect_of", "aspect_widen", "aspect_fov_angle", "host_aspect", "aspect_ui_fit")
    )
    code += r"""
int main(int argc, char **argv) {
    if (argc != 6 && argc != 9) return 2;
    choice = argv[1]; width = atoi(argv[2]); height = atoi(argv[3]); version.number = atoi(argv[4]);
    version.aspect_first_lr = version.number == 100 ? 0x460d44 : version.number == 180 ? 0x8a6d8 : 0;
    float a = aspect_of(), scale = a ? a / (16.0f / 9) : 1;
    float fov[4] = {strtof(argv[5], 0), 0, 0, 0};
    if (argc == 9) {
        float projection[64] = {0};
        float *fields = projection + 0xa0 / sizeof(float);
        fields[0] = fov[0];
        fields[1] = sinf(fields[0] / 2); fields[2] = cosf(fields[0] / 2); fields[3] = tanf(fields[0] / 2);
        Cpu c = {0};
        int second = atoi(argv[6]);
        c.x[30] = rt_main_addr(second ? 0x460e00 : version.aspect_first_lr);
        c.x[second ? 11 : 8] = (uint64_t)(uintptr_t)projection;
        aspect_scale = scale;
        float w = strtof(argv[7], 0), h = strtof(argv[8], 0), result;
        fields[4] = w / h;
        for (int i = 0; i < 1000; i++) {
            memcpy(&c.v[0], &w, sizeof w); memcpy(&c.v[1], &h, sizeof h);
            host_aspect(&c);
            memcpy(&fields[4], &c.v[0], sizeof(float)); // the following guest store
        }
        memcpy(&result, &c.v[0], sizeof result);
        printf("%.9g %.9g %.9g %.9g %.9g\n", result, fields[0], fields[1], fields[2], fields[3]);
        return 0;
    }
    float m[16] = {1, 0, 0, 0.25f, 0, 16.0f / 9, 0, 0.5f, 0, 0, -1, -1, 0, 0, -1, 0};
    if (scale < 1) fov[0] = aspect_fov_angle(fov[0], scale);
    fov[1] = sinf(fov[0] / 2); fov[2] = cosf(fov[0] / 2); fov[3] = tanf(fov[0] / 2);
    aspect_ui_fit(m, scale);
    printf("%.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g\n", a, fov[0], fov[1], fov[2], fov[3], m[0], m[5], m[7]);
}
"""
    d = tmp_path_factory.mktemp("aspect")
    (d / "aspect.c").write_text(code)
    exe = d / "aspect.exe"
    subprocess.run(
        [
            cc,
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-D_CRT_SECURE_NO_WARNINGS",  # Windows CI: the UCRT deprecates sscanf and friends
            "-I" + str(ROOT / "runtime"),
            str(d / "aspect.c"),
            *LIBM,
            "-o",
            str(exe),
        ],
        check=True,
    )
    return exe


def run(aspect, choice, w, h, version=100, angle=1.0):
    result = subprocess.run(
        [str(aspect), choice, str(w), str(h), str(version), str(angle)], check=True, capture_output=True, text=True
    )
    return [float(x) for x in result.stdout.split()]


@pytest.mark.parametrize(("w", "h"), [(2448, 1848), (1968, 1248), (1280, 800), (1200, 1600), (1080, 1920)])
@pytest.mark.parametrize("angle", [0.4, 1.0, 1.6])
def test_tall_window_preserves_horizontal_fov_and_fits_ui(aspect, w, h, angle):
    a, requested, si, co, ta, mx, my, ty = run(aspect, "window", w, h, angle=angle)
    scale = (w / h) / (16 / 9)
    assert a == pytest.approx(w / h)
    assert requested == pytest.approx(2 * math.atan(math.tan(angle / 2) / scale))
    assert ta * a == pytest.approx(math.tan(angle / 2) * 16 / 9)
    assert si / co == pytest.approx(ta)
    assert si * si + co * co == pytest.approx(1)
    assert mx == 1
    assert my == pytest.approx(16 / 9 * scale)
    assert ty == pytest.approx(0.5 * scale)


@pytest.mark.parametrize("choice", ["16:9", "window"])
def test_16x9_is_unchanged(aspect, choice):
    a, requested, si, co, ta, mx, my, ty = run(aspect, choice, 1920, 1080)
    assert a == 0
    assert mx == 1 and my == pytest.approx(16 / 9) and ty == 0.5
    assert ta == pytest.approx(math.tan(0.5))


@pytest.mark.parametrize(
    "choice,w,h", [("21:9", 2520, 1080), ("32:9", 3840, 1080), ("48:9", 5760, 1080), ("window", 2560, 1080)]
)
def test_wide_keeps_existing_hor_plus(aspect, choice, w, h):
    a, requested, si, co, ta, mx, my, ty = run(aspect, choice, w, h)
    assert a == pytest.approx(w / h)
    assert ta == pytest.approx(math.tan(0.5))
    assert mx == pytest.approx((16 / 9) / (w / h))
    assert my == pytest.approx(16 / 9) and ty == 0.5


def test_180_fits_tall_window_too(aspect):
    assert run(aspect, "window", 2448, 1848, version=180)[0] == pytest.approx(2448 / 1848)


def test_version_without_fov_site_keeps_letterboxing(aspect):
    assert run(aspect, "window", 2448, 1848, version=999)[0] == 0


@pytest.mark.parametrize("w,h", [(0, 1848), (2448, 0), (-1, 1848)])
def test_invalid_drawable_does_not_enable_the_patch(aspect, w, h):
    assert run(aspect, "window", w, h)[0] == 0


@pytest.mark.parametrize("second", [0, 1])
@pytest.mark.parametrize("view_width,view_height", [(1280, 720), (640, 720), (640, 360)])
def test_aspect_sites_leave_layout_at_16x9_and_assign_race_aspect(aspect, second, view_width, view_height):
    result = subprocess.run(
        [str(aspect), "window", "2448", "1848", "100", "1.0", str(second), str(view_width), str(view_height)],
        check=True,
        capture_output=True,
        text=True,
    )
    a, requested, si, co, ta = [float(x) for x in result.stdout.split()]
    scale = (2448 / 1848) / (16 / 9)
    assert a == pytest.approx(view_width / view_height * (scale if second else 1))
    # The later race FOV hook is tested in test_vertplus; aspect assignment must not pre-write its caches.
    assert requested == 1
    assert ta == pytest.approx(math.tan(0.5))
    assert si / co == pytest.approx(ta)


def test_camera_hook_leaves_unmatched_menu_views_alone(aspect):
    result = subprocess.run(
        [str(aspect), "window", "2448", "1848", "100", "1.0", "0", "800", "600"],
        check=True,
        capture_output=True,
        text=True,
    )
    a, requested, si, co, ta = [float(x) for x in result.stdout.split()]
    assert a == pytest.approx(4 / 3)
    assert requested == 1
    assert ta == pytest.approx(math.tan(0.5))
