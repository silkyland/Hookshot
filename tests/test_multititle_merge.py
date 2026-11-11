"""Game-free regression checks for save UID calls and platform address-space policy.

python -I -B tests/test_multititle_merge.py [--cc cc]
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(source, marker):
    start = source.index(marker)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    fs = (ROOT / 'host/nn_fs.c').read_text()
    symbols = [
        '_ZN2nn2fs18IsSaveDataExistingENS_13ApplicationIdERKNS_7account3UidE',
        '_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE',
        '_ZN2nn2fs21MountSaveDataReadOnlyEPKcNS_13ApplicationIdERKNS_7account3UidE',
    ]
    common = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
'''
    save = common + r'''
#define MAX_PATH 256
#define HOS_TITLE_OOT 0
#define INVALID_FILE_ATTRIBUTES (~0u)
static int title;
static uint64_t hos_program_id = 0x0100123456789abcULL;
static const char *hos_user_dir = "user";
static char mounted[256], queried[256];
static unsigned char guest[0x200];
static uint64_t read_guest(uint64_t address) {
    CHECK(address >= 0x100 && address + 8 <= sizeof guest);
    uint64_t value; memcpy(&value, guest + address, 8); return value;
}
#define G64(a) read_guest(a)
static int hos_game_title(void) { return title; }
typedef struct { uint64_t x[8]; } Ctx;
#define HLE(sym) void imp_##sym(Ctx *c)
#define ARG(n) c->x[n]
#define RET(value) do { c->x[0] = (value); return; } while (0)
static const char *gstr(uint64_t address) { (void)address; return "save"; }
static void user_mount(const char *name, const char *sub) { (void)name; snprintf(mounted, sizeof mounted, "%s", sub); }
static unsigned GetFileAttributesA(const char *path) { snprintf(queried, sizeof queried, "%s", path); return 0; }
'''
    save += function(fs, 'static void save_sub(') + '\n'
    save += '\n'.join(function(fs, 'HLE(' + symbol + ')') for symbol in symbols)
    save += r'''
int main(void) {
    uint64_t uid[] = {0x123456789abcdef0ULL, 0xfedcba9876543210ULL};
    memcpy(guest + 0x100, uid, sizeof uid);
    Ctx c = {{0}};
'''
    save += f'''
    c.x[1] = 0x100; imp_{symbols[1]}(&c);
    CHECK(!strcmp(mounted, "save\\\\fedcba9876543210123456789abcdef0"));
    char original[256]; strcpy(original, mounted);
    c.x[2] = 0x100; imp_{symbols[2]}(&c); CHECK(!strcmp(mounted, original));
    c.x[1] = 0x100; imp_{symbols[0]}(&c);
    CHECK(!strcmp(queried + strlen("user\\\\"), original));
    title = 1;
    c.x[1] = 0x100; imp_{symbols[1]}(&c);
    CHECK(strcmp(mounted, original)); CHECK(strstr(mounted, "fedcba9876543210123456789abcdef0"));
    strcpy(original, mounted);
    c.x[2] = 0x100; imp_{symbols[2]}(&c); CHECK(!strcmp(mounted, original));
    c.x[1] = 0x100; imp_{symbols[0]}(&c);
    CHECK(!strcmp(queried + strlen("user\\\\"), original));
    puts("PASS: save UID address, legacy OoT path and cross-title isolation");
}}
'''
    mem = (ROOT / 'host/hos_mem.c').read_text()
    memory = common + r'''
#define LOCK() ((void)0)
#define UNLOCK() ((void)0)
#define HOS_SPACE_END (1ULL << 39)
#define HOS_HEAP_BASE (1ULL << 36)
static uint64_t hos_heap_size, guest_size = HOS_SPACE_END, region_floor = ~0ULL;
static uint64_t low_hole[2][2];
typedef struct { uint64_t addr, size; } Range;
static Range *regions;
static int nregions, capregions;
uint64_t hos_guest_size(void) { return guest_size; }
static void hos_commit(uint64_t address, uint64_t size) { (void)size; CHECK(address < guest_size); }
#define hos_fatal(...) abort()
'''
    memory += function(mem, 'uint64_t hos_nx2_region(') + '\n'
    memory += function(mem, 'uint64_t hos_region_alloc(') + '\n'
    memory += r'''
int main(void) {
    uint64_t size = 0, base = hos_nx2_region(&size);
#ifdef __ANDROID__
    CHECK(base == guest_size * 3 / 4 && size == guest_size / 4);
    CHECK(hos_region_alloc(0x10000, 0x10000) == base - 0x10000);
    guest_size = 1ULL << 37;
    base = hos_nx2_region(&size);
    CHECK(base == guest_size * 3 / 4 && base + size == guest_size);
    free(regions); regions = NULL; nregions = capregions = 0;
    CHECK(hos_region_alloc(0x10000, 0x10000) == base - 0x10000);
#else
    CHECK(base == 0x3000000000ULL && size == 0x2000000000ULL);
    CHECK(hos_region_alloc(0x10000, 0x10000) == HOS_SPACE_END - 0x10000);
#endif
    free(regions);
    puts("PASS: platform VAMM bounds and top-down region allocation");
}
'''
    with tempfile.TemporaryDirectory(prefix='multititle-merge-') as temporary:
        folder = Path(temporary)
        for name, source, flags in [('save', save, []), ('desktop-memory', memory, []),
                                    ('android-memory', memory, ['-D__ANDROID__'])]:
            src, exe = folder / (name + '.c'), folder / name
            src.write_text(source)
            subprocess.run([args.cc, '-std=c11', '-Wall', '-Wextra', '-Werror', *flags, str(src), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
