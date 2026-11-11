"""Physical-core masks follow Linux's sibling maps, including ARM clusters.

Compiles the real POSIX topology reader against a temporary sysfs tree.
python -I -B tests/test_cpu_topology.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    source = (ROOT / "host/posix/win32_posix.c").read_text(encoding="utf-8")
    reader = source[source.index("/* ---- processors ---- */"):source.index("void GetSystemInfo(")]
    fixture = r'''
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
using DWORD = uint32_t;
using ULONG_PTR = uintptr_t;
using BOOL = int;
constexpr BOOL TRUE = 1, FALSE = 0;
constexpr int RelationProcessorCore = 0;
#define ERROR_INVALID_PARAMETER 87
#define ERROR_NOT_SUPPORTED 50
struct SYSTEM_LOGICAL_PROCESSOR_INFORMATION {
    ULONG_PTR ProcessorMask;
    int Relationship;
    struct { unsigned char Flags; } ProcessorCore;
};
static DWORD last_error;
static long fake_cpu_count;
static const char *fixture_root;
static FILE *fixture_open(const char *path, const char *mode) {
    constexpr char prefix[] = "/sys/devices/system/cpu/";
    if (std::strncmp(path, prefix, sizeof(prefix) - 1)) std::abort();
    const std::string mapped = std::string(fixture_root) + "/" + (path + sizeof(prefix) - 1);
    return std::fopen(mapped.c_str(), mode);
}
#define fopen fixture_open
#define sysconf(option) fake_cpu_count
''' + reader + r'''
#undef fopen
#undef sysconf
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); return 1; } } while (0)
int main(int argc, char **argv) {
    if (argc < 4) return 2;
    fixture_root = argv[1];
    fake_cpu_count = std::strtol(argv[2], nullptr, 10);
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION info[64] = {};
    DWORD bytes = sizeof info;
    if (!std::strcmp(argv[3], "failure")) {
        CHECK(!GetLogicalProcessorInformation(info, &bytes));
        return 0;
    }
    std::vector<ULONG_PTR> expected;
    for (int i = 3; i < argc; ++i) expected.push_back(std::strtoull(argv[i], nullptr, 16));
    CHECK(GetLogicalProcessorInformation(info, &bytes));
    CHECK(bytes == expected.size() * sizeof info[0]);
    ULONG_PTR seen = 0;
    for (size_t i = 0; i < expected.size(); ++i) {
        CHECK(info[i].ProcessorMask == expected[i]);
        CHECK(info[i].Relationship == RelationProcessorCore);
        CHECK(info[i].ProcessorCore.Flags == (__builtin_popcountll(expected[i]) > 1));
        CHECK(!(seen & info[i].ProcessorMask));
        seen |= info[i].ProcessorMask;
    }
    DWORD needed = 0;
    CHECK(!GetLogicalProcessorInformation(nullptr, &needed));
    CHECK(needed == bytes);
    CHECK(last_error == 122);
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="cpu-topology-") as temporary:
        directory = Path(temporary)
        cpp, executable = directory / "fixture.cpp", directory / "fixture"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(executable)], check=True)

        def check(name, siblings, expected, *, compatibility=False, identities=None):
            tree = directory / name
            for cpu, listing in enumerate(siblings):
                topology = tree / f"cpu{cpu}/topology"
                topology.mkdir(parents=True)
                if listing is not None:
                    filename = "thread_siblings_list" if compatibility else "core_cpus_list"
                    (topology / filename).write_text(listing + "\n", encoding="ascii")
                package, core, cluster = identities[cpu] if identities else (0, cpu, 0)
                for field, value in [("physical_package_id", package), ("core_id", core), ("cluster_id", cluster)]:
                    (topology / field).write_text(str(value) + "\n", encoding="ascii")
            wanted = ["failure"] if expected is None else [f"{mask:x}" for mask in expected]
            subprocess.run([str(executable), str(tree), str(len(siblings)), *wanted], check=True)
            print("PASS:", name, flush=True)

        # Xiaomi Pad 7 Pro: same package/core IDs repeat across three clusters,
        # but there are eight separate, non-SMT cores. Previously grouped as
        # 0x89/0x12/0x24/0x40, reserving CPUs 0,3,7 instead of CPU7 alone.
        identities = [(0, 0, 0), (0, 1, 0), (0, 2, 0), (0, 0, 1),
                      (0, 1, 1), (0, 2, 1), (0, 3, 1), (0, 0, 2)]
        check("arm-clusters", [str(i) for i in range(8)], [1 << i for i in range(8)], identities=identities)
        check("legacy-sibling-name", ["0,4", "1,5", "2,6", "3,7"] * 2,
              [0x11, 0x22, 0x44, 0x88], compatibility=True,
              identities=[(0, i % 4, 0) for i in range(8)])
        check("ranges-and-commas", ["0-1,4-5", "0-1,4-5", "2-3,6-7", "2-3,6-7"] * 2,
              [0x33, 0xcc])
        check("full-mask-width", [str(i) for i in range(64)], [1 << i for i in range(64)])
        check("missing-topology", [None, None], None)
        check("empty-map", ["", "1"], None)
        check("map-omits-self", ["1", "0"], None)
        check("inconsistent-overlap", ["0-1", "1"], None)
        check("descending-range", ["1-0", "1"], None)
        check("outside-mask", ["0,64", "1"], None)
        check("trailing-junk", ["0x", "1"], None)
        check("trailing-comma", ["0,", "1"], None)
        check("truncated-map", ["0" * 600 + ",1", "1"], None)


if __name__ == "__main__":
    main()
