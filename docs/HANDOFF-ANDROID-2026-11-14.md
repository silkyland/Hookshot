# Linux handoff: video-lag A/B results and next steps

Continuation of [HANDOFF-ANDROID-2026-11-11.md](HANDOFF-ANDROID-2026-11-11.md).
Read it and [FINDINGS-ANDROID-2026-11-11.md](FINDINGS-ANDROID-2026-11-11.md) first; the earlier warning stands —
findings are notes, not conclusions. This file adds paired A/B measurements taken on the device today.

## Start here

```sh
git fetch origin
git switch codex/android-frame-budget   # or merge it into main first — it is 47b8454 + diagnostics
adb devices -l                          # tablet: Xiaomi Pad 7 Pro, serial ac9c8ddc
```

## New evidence (same device, same attract-movie scene `Dm_Title_0005`, 15 s windows)

Measured with `scripts/android/measure_frames.py` (SurfaceFlinger frame timestamps + per-thread CPU):

| Config | fps | median | p95 | frames >50 ms | total CPU | dav1d CPU |
|---|---|---|---|---|---|---|
| baseline env + movie | **21.0** | 41 ms | 108 ms | 94 | 354% | 153% |
| `HOS_FPS_TARGET=30` + movie | 28.2 | 33 ms | 75 ms | 69 | 404% | 208% |
| `HOS_RESERVE_CORE=0` + movie | **33.1** | **25 ms** | **58 ms** | **38** | 493% | 255% |
| `HOS_NO_MOVIES=1` (no video at all) | 32.7 | 33 ms | 42 ms | 3 | 266% | 0% |

What this proves and what it does not:

- The game is **CPU-bound at ~33 fps even with no movie**: the stutter is not "just the video".
  Video decode costs roughly a third of the framerate on top (21 vs 33 fps).
- `HOS_RESERVE_CORE=0` measurably helps the movie path (21 → 33 fps ≈ no-movie level).
  It does **not** cure; the game still cannot reach 60 fps on this SoC.
- `dav1d-worker` holds 153–255% CPU with 0.7–1.5 s/s of runqueue wait; `ModuleSystemWor` and
  `sead::MainThread` are equally starved (300 ms–1.6 s/s runqueue). Classic oversubscription.
- GPU is not the wall: device-wide GPU busy 16–46%, `vk: GPU time` 1.3–3.6 ms/frame.
- No hardware AV1 decoder exists on this tablet — only `c2.android.av1.decoder` (libgav1) and
  `c2.android.av1-dav1d.decoder`, both software. `HOS_AV1_MC=1` can never beat bundled dav1d here.
- One sample per config; thermal drift between successive runs is possible. Re-run on Linux
  before treating the deltas as exact.

## Device state right now

- `hookshot_env.txt` on the tablet = original six lines **plus `HOS_RESERVE_CORE=0`** (left in place:
  it measurably helps; revert by deleting the line and force-stopping the app).
- Installed APK still has the **old 5-core topology bug** (game threads pinned to CPUs 1,2,4,5,6).
  The fix is in `main` (`win32_posix.c` sibling maps) but has never been built into an APK.
- `dav1d` is built with `n_threads=0` = all cores. No env knob exists to cap it — see below.

## What to do on Linux (needs `~/.cache/hookshot-builder` gen/ + exefs — absent on macOS)

1. **Build the current `main` APK** — it already contains the CPU-topology fix. Install it, then
   re-run the A/B: baseline vs `HOS_RESERVE_CORE` unset (fixed topology = 7 cores + reserved replay
   core) vs `HOS_RESERVE_CORE=0`. Decide which affinity to ship from numbers, not preference.
2. **Add a `HOS_AV1_THREADS` knob** in `host/android/android_media_av1.c` `mf_av1_open()`
   (`dav1d_default_settings(&s); s.n_threads = ...`). The bench decodes ~140 fps with 8 threads;
   try 4–6 and measure whether game threads stop starving while the movie still sustains 60 fps.
   This is the single most promising lever: it reduces the contention that `RESERVE_CORE=0`
   currently papers over by oversubscribing.
3. While in the file, the MediaCodec path's known bugs (no input copy, software-first selection)
   are only worth fixing for correctness — this device has no AV1 hardware decoder to win from.
4. Re-verify with `measure_frames.py`: movie-enabled run must beat 33 fps and cut `over_50ms`
   materially before claiming the video-lag fixed.

## Do not redo

- `HOS_FPS_TARGET=30` alone: median pins at 33 ms but tail stays awful (p95 75 ms) — cosmetic.
- `HOS_NO_MOVIES=1`: diagnostic only, removes the feature.
- Surface downsizing / stock Vulkan / vcore-off / affinity 0–6 workarounds / logcat: all tried
  by the earlier session, none cured (details in the Nov-11 findings).
- Do not infer hardware decode from `HOS_AV1_MC=1` failures: the path is known-broken and the
  device has no hardware codec anyway.

## Tooling on this branch

- `scripts/android/measure_frames.py --serial ac9c8ddc --seconds 15 --target-fps 60 --out <dir>`
  exits non-zero when the frame budget fails (that's normal — the game is below 60 fps).
- `scripts/android/run_diagnostics.py`, `build_diagnostics.py`, `build_dav1d_bench.py`,
  `dav1d_bench.c`, `android-diagnostics/` — the standalone probe APK (`dev.hookshot.diagnostics`,
  already installed on the tablet).
- `tests/test_android_frame_budget.py` — 6 unit tests for the measurement logic, all passing.
- `android-common/.../ControllerOverlay.java` — user WIP, unchanged from the earlier session.
