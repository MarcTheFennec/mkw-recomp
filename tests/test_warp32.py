"""Run the translator's GLSL warp helpers as C++ with synthetic 32/64/128-lane subgroups, without a GPU or game.

Each logical warp has different values and ballot bits, so accidentally reading a neighbouring warp is observable.
Usage: uv run pytest tests/test_warp32.py
"""

import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "sm86/translate"))
from glsl import common  # noqa: E402
from glsl.thread import ThreadOps  # noqa: E402

HARNESS = r"""
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <limits>
using uint = uint32_t;
uint gl_SubgroupInvocationID, gl_SubgroupSize, values[128];
struct uvec4 { uint v[4]; uint operator[](uint i) const { return v[i]; } };
struct uvec2 { uint x, y; };
uvec4 predicate_mask, active_mask;
int ballot_calls;
uint subgroupShuffle(uint, uint from) { return values[from]; }
uvec2 subgroupShuffle(uvec2, uint from) { return {values[from], values[from] + 10000}; }
uvec4 subgroupBallot(bool) { return ballot_calls++ ? active_mask : predicate_mask; }
"""


def run_helpers(tmp_path: Path, body: str) -> None:
    cc = shutil.which("clang++") or shutil.which("c++") or shutil.which("g++")
    if not cc:
        pytest.skip("no C++ compiler")
    src, exe = tmp_path / "warp.cpp", tmp_path / "warp.exe"
    helpers = getattr(common, "WARP_FN", "") + "\n" + common.SHFL_FN.replace("out bool ok", "bool &ok")
    src.write_text(HARNESS + helpers + body)
    subprocess.run([cc, "-std=c++17", "-O1", str(src), "-o", str(exe)], check=True)
    p = subprocess.run([str(exe)], capture_output=True, text=True)
    assert p.returncode == 0, p.stderr


def test_shuffles_do_not_cross_32_lane_warps(tmp_path: Path) -> None:
    run_helpers(tmp_path, r"""
int main() {
    const uint controls[] = {31, 15, 7, 0, 0x101f, 0x81f, 0x41f, 0x1f1f};
    for (uint size : {32u, 64u, 128u}) {
        gl_SubgroupSize = size;
        for (uint k = 0; k < size; k++) values[k] = (k / 32) * 1000 + k % 32;
        for (uint c : controls) for (uint mode = 0; mode < 4; mode++) for (uint src = 0; src < 40; src++) {
            for (uint lane = 0; lane < size; lane++) {
                bool reference_ok, actual_ok;
                gl_SubgroupInvocationID = lane % 32;
                uint reference = shfl(values[lane % 32], src, c, mode, reference_ok);
                gl_SubgroupInvocationID = lane;
                uint actual = shfl(values[lane], src, c, mode, actual_ok);
                uint expected = reference + (lane / 32) * 1000;
                if (actual != expected || actual_ok != reference_ok) {
                    std::fprintf(stderr, "size %u lane %u mode %u: got %u, expected %u\n", size, lane, mode, actual, expected);
                    return 1;
                }
            }
        }
    }
}
""")


def test_ballots_and_votes_use_the_callers_warp(tmp_path: Path) -> None:
    assert hasattr(common, "WARP_FN"), "wide subgroups need logical-warp ballot helpers"
    run_helpers(tmp_path, r"""
int main() {
    for (uint size : {32u, 64u, 128u}) {
        gl_SubgroupSize = size;
        for (uint pattern = 0; pattern < 4; pattern++) {
            for (uint w = 0; w < 4; w++) {
                active_mask.v[w] = w * 32 < size ? (w & 1 ? 0xf0ff00ffu : 0x7fffffffu) : 0;
                predicate_mask.v[w] = pattern == 0 ? 0 : pattern == 1 ? active_mask.v[w]
                    : (pattern == 2 ? 0x55555555u : (w & 1 ? 0xffffffffu : 0)) & active_mask.v[w];
            }
            for (uint lane = 0; lane < size; lane++) {
                uint w = lane / 32, active = active_mask.v[w], yes = predicate_mask.v[w];
                if (!(active >> (lane % 32) & 1)) continue;
                gl_SubgroupInvocationID = lane;
                bool p = yes >> (lane % 32) & 1;
                ballot_calls = 0;
                if (warp_ballot(p) != yes) return 1;
                ballot_calls = 0;
                if (warp_all(p) != (yes == active)) return 2;
                ballot_calls = 0;
                if (warp_any(p) != (yes != 0)) return 3;
                ballot_calls = 0;
                if (warp_equal(p) != (yes == 0 || yes == active)) return 4;
            }
        }
    }
}
""")


def test_emitted_reductions_respect_warp_and_signedness(tmp_path: Path) -> None:
    class Instruction:
        def __init__(self, operation: str, signed: bool):
            self.operation, self.signed = operation, signed

        def sym(self, name: str) -> str:
            return self.operation if name == "op" else "S32" if self.signed else "U32"

    class Builder(ThreadOps):
        def src_u(self, instruction, operand):
            return "values[gl_SubgroupInvocationID]"

        def dst(self, instruction, expression):
            return f"result = {expression};"

    native = r"""
template<class T> T min(T a, T b) { return a < b ? a : b; }
template<class T> T max(T a, T b) { return a > b ? a : b; }
bool active(uint k) { return active_mask.v[k / 32] >> (k % 32) & 1; }
template<class T> T subgroupAdd(T) { uint r=0; for(uint k=0;k<gl_SubgroupSize;k++) if(active(k)) r+=values[k]; return T(r); }
template<class T> T subgroupMin(T) { T r=std::numeric_limits<T>::max(); for(uint k=0;k<gl_SubgroupSize;k++) if(active(k)) r=min(r,T(values[k])); return r; }
template<class T> T subgroupMax(T) { T r=std::numeric_limits<T>::lowest(); for(uint k=0;k<gl_SubgroupSize;k++) if(active(k)) r=max(r,T(values[k])); return r; }
template<class T> T subgroupAnd(T) { uint r=~0u; for(uint k=0;k<gl_SubgroupSize;k++) if(active(k)) r&=values[k]; return T(r); }
template<class T> T subgroupOr(T) { uint r=0; for(uint k=0;k<gl_SubgroupSize;k++) if(active(k)) r|=values[k]; return T(r); }
template<class T> T subgroupXor(T) { uint r=0; for(uint k=0;k<gl_SubgroupSize;k++) if(active(k)) r^=values[k]; return T(r); }
"""
    operations = ["SUM", "MIN", "MAX", "AND", "OR", "XOR"]
    functions, calls = [], []
    for signed in [False, True]:
        for index, operation in enumerate(operations):
            name = f"reduce_{operation}_{int(signed)}"
            code = Builder().redux(Instruction(operation, signed))
            functions.append(f"uint {name}() {{ uint result = 0; {code} return result; }}")
            calls.append(f"{{ ballot_calls=1; uint got={name}(), expected=reference(lane,{index},{int(signed)}); "
                         f'if(got!=expected) {{ std::fprintf(stderr,"{name}: size %u lane %u got %u expected %u\\n",'
                         "size,lane,got,expected); return 1; } }")
    reference = r"""
uint reference(uint lane, uint op, bool sign) {
    uint result = 0; bool first = true;
    for(uint k=(lane/32)*32;k<(lane/32+1)*32;k++) if(active(k)) {
        uint x=values[k];
        if(first) { result=x; first=false; continue; }
        switch(op) {
        case 0: result+=x; break;
        case 1: if(sign ? int(x)<int(result) : x<result) result=x; break;
        case 2: if(sign ? int(x)>int(result) : x>result) result=x; break;
        case 3: result&=x; break;
        case 4: result|=x; break;
        case 5: result^=x; break;
        }
    }
    return result;
}
int main() {
    for(uint size : {32u,64u,128u}) {
        gl_SubgroupSize=size;
        for(uint w=0;w<4;w++) active_mask.v[w]=w*32<size ? (w&1 ? 0xf0ff00ffu : 0x7fffffffu) : 0;
        for(uint k=0;k<size;k++) values[k]=k%7==0 ? 0xfffffff0u+k : k*17+(k/32)*1000;
        for(uint lane=0;lane<size;lane++) if(active(lane)) {
            gl_SubgroupInvocationID=lane;
"""
    run_helpers(tmp_path, native + "\n".join(functions) + reference + "\n".join(calls) + "\n}}}\n")
