#!/bin/sh
# (android) Push the game payload into the debug app's files dir.
#
#   ./sideload.sh [game-folder] [adb-device]
#   PKG=dev.hookshot.pokopia GAME=~/Games/Hookshot-Pokopia ./sideload.sh
#
# game-folder: a Linux Hookshot game folder (default ~/Games/Hookshot, or $GAME) — needs image.bin and
#   romfs/; the small caches (hookshot_programs.bin, language.txt, hookshot_env.txt, ...) go along when
#   present. ~7-27 GB: use a USB cable or adb over Wi-Fi; this takes a while.
# The app must be installed first (assembleDebug + installDebug, or Android Studio's Run).
set -e
GAME="${1:-${GAME:-$HOME/Games/Hookshot}}"
DEV="${2:-${DEV:-}}"
ADB="adb ${DEV:+-s $DEV}"
PKG="${PKG:-dev.hookshot.oot}"
DST=/data/data/$PKG/files/hookshot

[ -f "$GAME/image.bin" ] || { echo "no image.bin in $GAME"; exit 1; }
[ -d "$GAME/romfs" ] || { echo "no romfs/ in $GAME"; exit 1; }

$ADB shell "run-as $PKG mkdir -p $DST/user $DST/romfs" 2>/dev/null || {
    echo "run-as failed: is the debug APK installed (assembleDebug + installDebug)?"; exit 1; }

echo "== image.bin + small files"
$ADB push "$GAME/image.bin" /data/local/tmp/hookshot-image.bin
$ADB shell "run-as $PKG sh -c 'cat /data/local/tmp/hookshot-image.bin > $DST/image.bin'"
$ADB shell rm /data/local/tmp/hookshot-image.bin
for f in hookshot_programs.bin hookshot_shaders.bin hookshot_pipelines.bin language.txt hookshot_env.txt; do
    if [ -f "$GAME/$f" ]; then
        $ADB push "$GAME/$f" "/data/local/tmp/hookshot-$f"
        $ADB shell "run-as $PKG sh -c 'cat /data/local/tmp/hookshot-$f > $DST/$f'"
        $ADB shell rm "/data/local/tmp/hookshot-$f"
    fi
done

echo "== romfs (the long part: streamed, no temp copy)"
tar -C "$GAME/romfs" -cf - . | $ADB shell "run-as $PKG sh -c 'cd $DST/romfs && tar xf -'"
$ADB shell "run-as $PKG ls $DST | head"
echo "done — launch the app"
