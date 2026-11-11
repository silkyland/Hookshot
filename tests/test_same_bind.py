"""Rebinding the same program, buffers, or handles does not drop the draw's descriptors.

python -I -B tests/test_same_bind.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def before(text, start, end, early, late):
    part = text[text.index(start):text.index(end)]
    if part.index(early) > part.index(late):
        raise SystemExit(f"{start} bumps the bindings before checking they changed")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    source = (ROOT / "gpu" / "vk" / "vk_replay.cpp").read_text(encoding="utf-8")
    handles = block(source, "bool handles_unchanged(")
    bufs = block(source, "bool buf_unchanged(")
    before(source, "case NOP_BIND_TEX:", "case NOP_STENCIL:", "handles_unchanged(", "s.bind_gen++")
    before(source, "case NOP_BIND_UBO:", "case NOP_BIND_TEX:", "buf_unchanged(", "s.bind_gen++")
    before(source, "case NOP_BIND_BUFS:", "case NOP_BIND_UBO:", "buf_unchanged(", "s.bind_gen++")
    program_bind = block(source, "case NOP_BIND_PROGRAM:")
    fixture = f'''#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define _Static_assert static_assert
#include "{ROOT / 'gpu/render/rx_nvn.h'}"
#define CHECK(x) do {{ if (!(x)) {{ std::fprintf(stderr, "FAIL:%d %s\\n", __LINE__, #x); std::exit(1); }} }} while (0)
{handles}
{bufs}
// Only stage-control decoding and stream framing are fixtures; execute the real replay case.
uint8_t nvn_stage_id(uint64_t control) {{ return control ? uint8_t(control - 1) : 0xff; }}
template<class T> T payload(const void* a, size_t avail) {{
    CHECK(avail == sizeof(T));
    T value; std::memcpy(&value, a, sizeof value); return value;
}}
struct BindState {{
    nvn_prog prog{{}};
    uint8_t prog_stage[6]{{}};
    bool has_prog = false;
    uint64_t bind_gen = 1;
}};
void bind(BindState& s, const nvn_prog* prog, int32_t stages) {{
    const nvn_p_bind_program record{{prog, stages}};
    const void* a = &record;
    const size_t avail = sizeof record;
    switch (int(NOP_BIND_PROGRAM)) {{
        {program_bind}
    }}
}}
int main() {{
    const uint64_t have[] = {{1, 2, 3}};
    const uint64_t next[] = {{1, 2, 4}};
    CHECK(handles_unchanged(have, have, 3));
    CHECK(handles_unchanged(have, next, 0));
    CHECK(!handles_unchanged(have, next, 3));
    CHECK(!handles_unchanged(have, next, -1));
    CHECK(buf_unchanged(1, 2, 0x1000, 64, 1, 2, 0x1000, 64));
    CHECK(!buf_unchanged(1, 2, 0x1000, 64, 1, 2, 0x2000, 64));
    CHECK(!buf_unchanged(1, 2, 0x1000, 64, 1, 3, 0x1000, 64));
    const nvn_prog vertex{{NVN_MAGIC('P', 'R', 'O', 'G'), 1, {{{{100, 1}}}}, 10}};
    const nvn_prog fragment{{NVN_MAGIC('P', 'R', 'O', 'G'), 1, {{{{200, 2}}}}, 20}};
    const nvn_prog unknown{{NVN_MAGIC('P', 'R', 'O', 'G'), 1, {{{{300, 0}}}}, 30}};
    BindState s;
    bind(s, &vertex, 1);
    CHECK(s.has_prog && s.prog.nstage == 1 && s.prog.sd[0].data == 100);
    const uint64_t vertex_gen = s.bind_gen;
    bind(s, &vertex, 1);
    CHECK(s.bind_gen == vertex_gen);
    bind(s, &fragment, 2);
    CHECK(s.bind_gen == vertex_gen + 1 && s.prog.nstage == 2);
    CHECK(s.prog.sd[0].data == 100 && s.prog.sd[1].data == 200);
    const uint64_t merged_gen = s.bind_gen, merged_serial = s.prog.serial;
    bind(s, &vertex, 1);
    bind(s, &fragment, 2);
    CHECK(s.bind_gen == merged_gen && s.prog.serial == merged_serial);
    BindState reverse;
    bind(reverse, &fragment, 2); bind(reverse, &vertex, 1);
    CHECK(reverse.prog.serial == merged_serial);
    nvn_prog replacement = fragment;
    replacement.sd[0].data = 201;
    bind(s, &replacement, 2);
    CHECK(s.bind_gen == merged_gen + 1 && s.prog.sd[0].data == 100 && s.prog.sd[1].data == 201);
    bind(s, nullptr, 2);
    CHECK(s.has_prog && s.prog.nstage == 1 && s.prog.sd[0].data == 100);
    const uint64_t unbound_gen = s.bind_gen;
    bind(s, nullptr, 2);
    CHECK(s.bind_gen == unbound_gen);
    bind(s, nullptr, 1);
    CHECK(!s.has_prog && s.prog.nstage == 0 && s.bind_gen == unbound_gen + 1);
    bind(s, nullptr, 1);
    CHECK(s.bind_gen == unbound_gen + 1);
    bind(s, &vertex, 1); bind(s, &unknown, 2);
    CHECK(s.has_prog && s.prog.nstage == 1 && s.prog.sd[0].data == 300 && s.prog_stage[0] == 0xff);
    const uint64_t unknown_gen = s.bind_gen;
    bind(s, &unknown, 2);
    CHECK(s.bind_gen == unknown_gen);
    std::printf("PASS: the same binding does not count as a new one\\n");
    std::printf("PASS: stage merges, replacements, unbinds and unknown-stage fallback\\n");
}}
'''
    with tempfile.TemporaryDirectory(prefix="same-bind-") as tmp:
        cpp = Path(tmp) / "fixture.cpp"
        exe = Path(tmp) / "fixture"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
