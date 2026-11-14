#!/usr/bin/env python3
"""Compile the real Android decoder functions + bundled library into a device CLI probe."""
import argparse
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]


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
    parser.add_argument('--cc', type=Path, required=True, help='NDK aarch64-linux-android30-clang')
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--shared', action='store_true', help='Build the diagnostics APK JNI library')
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    source = (ROOT / 'host/android/android_media_av1.c').read_text()
    code = '\n'.join(function(source, marker) for marker in (
        'static void d1_picture(', 'static void d1_drain(', 'static int d1_decode('))
    (args.out / 'game_dav1d.inc').write_text(code)
    binary = args.out / ('libdav1d_bench.so' if args.shared else 'dav1d-bench')
    flags = ['-shared', '-fPIC', '-DHOOKSHOT_BENCH_JNI', '-D_GNU_SOURCE', '-Wl,-z,max-page-size=16384'] if args.shared else []
    subprocess.run([str(args.cc), '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-O2', *flags, '-Wall', '-Wextra', '-Werror',
                    '-I' + str(args.out), '-I' + str(ROOT / 'third_party/dav1d/include'),
                    str(ROOT / 'scripts/android/dav1d_bench.c'),
                    str(ROOT / 'third_party/dav1d/lib/arm64-v8a/libdav1d.a'), '-lm', '-o', str(binary)], check=True)
    print(binary)


if __name__ == '__main__':
    main()
