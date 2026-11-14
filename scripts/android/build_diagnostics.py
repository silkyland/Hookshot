#!/usr/bin/env python3
"""Build an isolated controller preview / foreground bundled-AV1 benchmark APK.

Requires SDK 36, build-tools 36.1.0, NDK 27.2.12479018 and JDK17+. Output and
local diagnostic signing key stay under --out, outside the source checkout.
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', type=Path, required=True)
    parser.add_argument('--cc', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    out = args.out.resolve()
    if out.is_relative_to(ROOT):
        parser.error('--out must be outside the repository (contains APKs and a signing key)')
    tools, android = args.sdk / 'build-tools/36.1.0', args.sdk / 'platforms/android-36/android.jar'
    classes, dex = out / 'classes', out / 'dex'
    classes.mkdir(exist_ok=True); dex.mkdir(exist_ok=True)
    env = dict(os.environ)

    def run(*command):
        subprocess.run(list(map(str, command)), check=True, env=env)

    run(sys.executable, '-I', '-B', ROOT / 'scripts/android/build_dav1d_bench.py', '--cc', args.cc, '--out', out, '--shared')
    shared = ROOT / 'android-common/src/main/java/dev/hookshot/android'
    sources = [shared / (name + '.java') for name in ('ControllerModel', 'ControllerOverlay', 'ControllerWindow')]
    sources += sorted((ROOT / 'android-diagnostics/src').rglob('*.java'))
    run('javac', '--release', '17', '-classpath', android, '-d', classes, *sources)
    run(tools / 'd8', '--lib', android, '--min-api', '30', '--output', dex, *sorted(classes.rglob('*.class')))
    run(tools / 'aapt2', 'link', '-o', out / 'unsigned.apk', '-I', android, '--manifest', ROOT / 'android-diagnostics/AndroidManifest.xml')
    with zipfile.ZipFile(out / 'unsigned.apk', 'a', compression=zipfile.ZIP_DEFLATED) as apk:
        apk.write(dex / 'classes.dex', 'classes.dex')
        apk.write(out / 'libdav1d_bench.so', 'lib/arm64-v8a/libdav1d_bench.so')
    run(tools / 'zipalign', '-f', '4', out / 'unsigned.apk', out / 'aligned.apk')
    key = out / 'diagnostic.keystore'
    if not key.exists():
        run('keytool', '-genkeypair', '-keystore', key, '-storepass', 'android', '-keypass', 'android',
            '-alias', 'diagnostic', '-keyalg', 'RSA', '-keysize', '2048', '-validity', '3650', '-dname', 'CN=Local Hookshot Diagnostics')
    apk = out / 'hookshot-diagnostics.apk'
    run(tools / 'apksigner', 'sign', '--ks', key, '--ks-pass', 'pass:android', '--out', apk, out / 'aligned.apk')
    run(tools / 'apksigner', 'verify', apk)
    print(apk)


if __name__ == '__main__':
    main()
