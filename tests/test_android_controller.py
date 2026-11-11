"""Run the real portable touch model and JNI state bridge without game assets or the NDK.

python -I -B tests/test_android_controller.py [--cc clang] [--touch-source /path/to/mutated.c]
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--touch-source", type=Path, default=ROOT / "host/android/android_touch.c")
    args = parser.parse_args()
    settings = subprocess.run(["java", "-XshowSettings:properties", "-version"], capture_output=True, text=True, check=True)
    home = Path(re.search(r"java.home\s*=\s*(.+)", settings.stderr)[1].strip())
    java, javac = home / "bin/java", home / "bin/javac"
    shared = ROOT / "android-common/src/main/java/dev/hookshot/android"
    tests = ROOT / "tests/android_controller"
    with tempfile.TemporaryDirectory(prefix="android-controller-") as temporary:
        build = Path(temporary)
        subprocess.run([str(javac), "--release", "17", "-d", str(build), str(shared / "ControllerModel.java"),
                        str(shared / "TouchBridge.java"), str(tests / "ControllerModelTest.java"), str(tests / "NativeTouchTest.java")], check=True)
        subprocess.run([str(java), "-cp", str(build), "dev.hookshot.android.ControllerModelTest"], check=True, timeout=20)
        platform = "darwin" if sys.platform == "darwin" else "linux"
        library = build / ("touch.dylib" if platform == "darwin" else "touch.so")
        flags = ["-dynamiclib"] if platform == "darwin" else ["-shared", "-fPIC"]
        subprocess.run([args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pthread", *flags,
                        "-I" + str(home / "include"), "-I" + str(home / "include" / platform),
                        str(args.touch_source), str(tests / "native_touch_test.c"), "-o", str(library)], check=True)
        subprocess.run([str(java), "--enable-native-access=ALL-UNNAMED", "-cp", str(build),
                        "dev.hookshot.android.NativeTouchTest", str(library)], check=True, timeout=20)


if __name__ == "__main__":
    main()
