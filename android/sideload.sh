#!/bin/sh
# (android) Push the game payload into the debug app's files dir.
#
#   ./sideload.sh [game-folder] [adb-device]
#
# game-folder: a Linux Hookshot game folder (default ~/Games/Hookshot) — needs image.bin, romfs/ and
#   hookshot_shaders.bin if present. ~27 GB: use a USB cable or adb over Wi-Fi; this takes a while.
# The app must be installed first (assembleDebug + installDebug, or Android Studio's Run).
set -e
GAME="${1:-$HOME/Games/Hookshot}"
DEV="${2:-}"
ADB="adb ${DEV:+-s $DEV}"
PKG=dev.hookshot.oot
DST=/data/data/$PKG/files/hookshot

[ -f "$GAME/image.bin" ] || { echo "no image.bin in $GAME"; exit 1; }
[ -d "$GAME/romfs" ] || { echo "no romfs/ in $GAME"; exit 1; }

$ADB shell "run-as $PKG mkdir -p $DST/user $DST/romfs" 2>/dev/null || {
    echo "run-as failed: is the debug APK installed (assembleDebug + installDebug)?"; exit 1; }

echo "== image.bin + shaders"
$ADB push "$GAME/image.bin" /data/local/tmp/hookshot-image.bin
$ADB shell "run-as $PKG sh -c 'cat /data/local/tmp/hookshot-image.bin > $DST/image.bin'"
$ADB shell rm /data/local/tmp/hookshot-image.bin
if [ -f "$GAME/hookshot_shaders.bin" ]; then
    $ADB push "$GAME/hookshot_shaders.bin" /data/local/tmp/hookshot-shaders.bin
    $ADB shell "run-as $PKG sh -c 'cat /data/local/tmp/hookshot-shaders.bin > $DST/hookshot_shaders.bin'"
    $ADB shell rm /data/local/tmp/hookshot-shaders.bin
fi

echo "== romfs (the long part: streamed, no temp copy)"
tar -C "$GAME/romfs" -cf - . | $ADB shell "run-as $PKG sh -c 'cd $DST/romfs && tar xf -'"
$ADB shell "run-as $PKG ls $DST | head"
echo "done — launch the app"
