# Linux handoff: Android investigation and controller overlay

Checkpoint branch: **`codex/android-cpu-topology`**, based on `multi-title` commit
`fdf5875645306814845b4602c41fbbc8af8b6d72`. Fetch this branch explicitly; `main` is older and does not contain
the Android work. This is a checkpoint requested before moving from the Mac to Linux.

## Start here

```sh
git fetch origin
git switch --track origin/codex/android-cpu-topology
# If the local branch already exists: git switch codex/android-cpu-topology
adb devices -l
```

Read [the measured findings](FINDINGS-ANDROID-2026-11-11.md) before relying on older notes.
The user explicitly warns against treating previous findings as complete or unquestionable.

## What is implemented

1. CPU topology correction in `host/posix/win32_posix.c`, with `tests/test_cpu_topology.py` and a saved CI patch.
   The kernel's sibling maps replace ambiguous package/core IDs. This is independently reviewed and passes
   13 regression cases. It is not installed in the tablet's game APK and does not claim to solve all stutter.
2. **Controller overlay WIP**, shared by both Android APK projects:
   - `android-common/src/main/java/dev/hookshot/android/ControllerModel.java`: shared layout and pointer ownership.
   - `ControllerOverlay.java`: native Android buttons and two drawn analog sticks, pressed feedback, Hide/Show,
     multi-pointer state, and accessibility labels/actions.
   - `ControllerWindow.java`: activity-owned transparent, non-focusable PopupWindow. NativeActivity owns the
     Vulkan window surface, so ordinary drawing into its decor surface is not a valid overlay strategy.
   - `HookshotActivity.java`: NativeActivity subclass and lifecycle cleanup. Both manifests now select it,
     enable Java code, and both Gradle projects include `android-common` sources.
   - `TouchBridge.java` and `host/android/android_touch.c`: JNI publishes coherent button/axis snapshots.
     The old invisible touch mapper self-deadlocked on its mutex and had unreachable/overlapping zones.
   - `android_input.c`/`android_main.c`: register `onInputEvent` with native_app_glue instead of competing
     manual queue readers; reset held input on focus loss/pause. Physical pad state still merges with touch.
3. Portable model/JNI tests in `tests/android_controller/` and `tests/test_android_controller.py`.

### Overlay verification status — do not overstate

- Portable model tests pass: layout bounds/48dp targets at tested landscape sizes, all 17 physical pad bits,
  Nintendo default face mapping, simultaneous sticks/buttons, individual pointer release, cancellation,
  hiding, resizing and radial clamp.
- The real C JNI bridge compiles and runs in a host JVM: range clamping, physical-input merging,
  disabled/reset state and concurrent coherent publication/read pass.
- All shared Android Java sources in this checkpoint compile against the local Android 36 SDK with
  `javac --release 17`; re-run the build with the Linux toolchain.
- **No controller preview APK has been built or installed yet. No integrated game APK has been built or
  installed with the overlay. No on-device overlay touch/visual QA has happened.** Work was interrupted
  to commit/push the requested checkpoint.
- Review of the new overlay was interrupted and is incomplete at checkpoint time. Review lifecycle, PopupWindow behavior,
  physical key handling, JNI loading, manifest/source-set integration and input-queue ownership.
- CPU topology CI wiring is saved in `docs/android-cpu-topology-ci.patch` instead of applied to the
  workflow: GitHub rejected the checkpoint push because the current OAuth credential lacks `workflow`
  scope. Apply it later with `git apply docs/android-cpu-topology-ci.patch` and commit/push using a
  credential allowed to update workflows. The CPU regression test itself is committed and runnable.
- `tests/test_android_controller.py` is not yet wired into CI. It needs JDK17+ and a C compiler.

The overlay is intended to be event-driven; avoid a continuous redraw loop or per-frame file logging.
Do not add a system-wide overlay/accessibility service or new Android permissions just to implement controls.
Use the same model for the visible controls and hit areas. Keep Nintendo labels aligned with the default
raw pad bindings (`A=HOS_PAD_EAST`, `B=SOUTH`, `X=NORTH`, `Y=WEST`); physical remapping currently affects touch too.

## Verify and finish the controller

```sh
python3 -I -B tests/test_cpu_topology.py --cxx g++
python3 -I -B tests/test_android_controller.py --cc cc
git diff --check
```

Next create a separate test-only preview APK using the **actual shared overlay classes** and a test sink.
Use a distinct package ID, not `dev.hookshot.oot`. Validate multi-touch with MotionEvents delivered through
the actual widgets, individual lifts, ACTION_CANCEL, Hide/Show, focus loss and pause/resume. Inspect tablet
screenshots and a small landscape viewport. Confirm that the popup does not steal game focus or keyboard/
gamepad input. Verify text and pressed-state contrast, system/gesture insets and accessibility controls.

Then build the game libraries and integrated APK using the existing Linux build assets. Preserve native
symbol/JNI export loading: `HookshotActivity` loads `hookshot_game` before publishing through `TouchBridge`.
If extending tests, check unknown physical keys and Android Back/volume behavior rather than consuming
every key unconditionally. Do not silently mark these as verified.

## Linux build and payload constraints

The Mac did not have an NDK or generated game source cache; a full build was not attempted there.
Locate and reuse the Linux machine's existing build/generated artifacts before asking the user for anything.
Earlier machine paths (verify, do not assume):

- `/home/dit/oot-hookshot/build-android`
- `~/.cache/hookshot-builder/sa/gen`
- `~/.cache/hookshot-builder/sa/build/rt64`
- `~/Games/Hookshot`

`android/CMakeLists.txt` expects generated code, generated runtime, FFmpeg headers and shader dependencies.
It builds both `libhookshot_game.so` and `libhookshot_vk.so`; the POSIX topology reader is compiled into both.
Stage both matching libraries into `android-app/app/src/main/jniLibs/arm64-v8a/`, then build the APK.
See `android/CMakeLists.txt` and the historical build section of `HANDOFF-ANDROID.md` for commands, but
verify current paths and tools. A fresh source checkout alone is not the complete game build environment.

**Never uninstall `dev.hookshot.oot`, clear its data, delete generated/build artifacts or re-transfer the
entire payload unnecessarily.** The installed game has app-private payload/save data. Use the existing
signing key for an in-place update; a signing mismatch is not a reason to uninstall. If the key is absent,
report it and keep the installed game intact.

Old installed game launch component:

```sh
adb -s ac9c8ddc shell am start -W -n dev.hookshot.oot/android.app.NativeActivity
```

New source launch component **after building/installing this overlay APK**:

```sh
adb -s ac9c8ddc shell am start -W -n dev.hookshot.oot/dev.hookshot.android.HookshotActivity
```

## Continue performance investigation without repeating assumptions

- Use the actual file being played, captured from `/proc/<pid>/fd`, not an assumed attract movie.
- The same AV1 movie can render at 60 fps in Android's software codec app; this does not identify the
  bottleneck inside the game's separate decoder/copy/render/scheduling pipeline.
- Logging experiments did not establish a dominant logging bottleneck. CPU topology is wrong, but opening
  CPUs did not cure stutter. Keep those two statements separate.
- Preserve PurpleVK: stock Vulkan produced device loss in this investigation.
- The optional MediaCodec game path has known input-copy and hardware-classification bugs; see findings.
- Keep one variable per causal experiment. Record warm-cache state, scene/time, thermal state and frame
  timestamp distributions. Do not use CPU-cycle percentages as frame-time percentages.
- Restore temporary environment files and log redirections. Original settings are in the findings document.

## Copy/paste prompt

```text
Continue Hookshot Android on branch codex/android-cpu-topology (not main).
Read docs/HANDOFF-ANDROID-2026-11-11.md and docs/FINDINGS-ANDROID-2026-11-11.md first.
Treat the findings as bounded measurements, not a complete explanation of stutter.
Finish/review/test the shared Android controller overlay and its JNI/native input integration.
Run the portable CPU topology and controller tests, then build/test a separate preview and the real APK
using existing Linux generated/build assets. Keep the installed Zelda package/payload/saves intact;
never uninstall or clear data to bypass a signing mismatch. The new source activity is HookshotActivity.
The CPU topology fix is tested but not installed; the overlay is source WIP without on-device QA.
Continue timing the actual game's decode/copy/scheduling/present path; do not assume logcat or lack of
hardware AV1 explains everything. Preserve PurpleVK and restore experimental device settings.
```
